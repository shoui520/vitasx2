// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "common/SingleWaiterProgressEvent.h"
#include "common/Threading.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "VUmicro.h"
#include "vita/VitaGpuVuUniversalEpoch.h"
#include "vita/VitaGpuVuVifInput.h"

#include <array>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace VitaGpuVu
{
	class GeneratedLoopKernelTransaction;
	class GeneratedLoopKernelDeferredSuccessor;
	class GeneratedLoopKernelDeferredStores;
	struct GeneratedLoopKernelTransactionLayout;
	struct GeneratedLoopKernelFinalStateValues;
}

#define MTVU_LOG(...) do{} while(0)
//#define MTVU_LOG DevCon.WriteLn

// Notes:
// - This class should only be accessed from the EE thread...
// - buffer_size must be power of 2
// - ring-buffer has no complete pending packets when read_pos==write_pos
class VU_Thread final {
	static const s32 buffer_size = (_1mb * 16) / sizeof(s32);

	u32 buffer[buffer_size];
	// Note: keep atomic on separate cache line to avoid CPU conflict
	alignas(__cachelinesize) std::atomic<int> m_ato_read_pos; // Only modified by VU thread
	alignas(__cachelinesize) std::atomic<int> m_ato_write_pos;    // Only modified by EE thread
	alignas(__cachelinesize) int  m_read_pos; // temporary read pos (local to the VU thread)
	int  m_write_pos; // temporary write pos (local to the EE thread)
	Threading::WorkSema semaEvent;
	Threading::SingleWaiterProgressEvent m_ring_space_progress;
	std::atomic_bool m_shutdown_flag{false};
	// The payload ring is intentionally large enough for bursty VIF traffic,
	// but its byte capacity must not become an unbounded queue of guest VU
	// executions. Four retained journals give CPU1 useful overlap while keeping
	// input-visible guest progress close to the EE producer; the ordered GXM
	// owner may use fewer physical transactional slots.
	static constexpr u64 MaximumOutstandingVuExecutions = 4;
	// Bound physical GPU descriptor generations below the 128-slot transaction
	// owner. Host-only state formulas do not consume a transaction/output slot
	// and must not force an early GXM drain merely because they are ordered in
	// the same private architectural generation.
	//
	// Physical BSpline r155 showed that 48 entries force a drain before normal
	// EndScene retirement. The failed r157 widening retained heap-backed output
	// vectors and exhausted newlib; product journals now live in a fixed bounded
	// arena. Leave eight slots for private canaries/failure ownership while
	// allowing roughly three BSpline frames to overlap without a CPU1 drain.
	static constexpr u32 MaximumGeneratedLoopKernelInFlightExecutions = 120;
	static constexpr u32 MaximumGeneratedLoopKernelInFlightContinuations = 48;
	// Move bounded generated runs to MTGS while CPU1 continues constructing the
	// same private generation.  Publication only transfers ordered descriptors;
	// it does not request vertex visibility or create a mid-scene firmware job.
	// Normal EndScene retirement remains authoritative, while the existing
	// pressure/observer path may still request an early visibility boundary.
	// Keep the asynchronous threshold above one observed BSpline frame-scale
	// burst. VSync now supplies the mandatory strong ordered close; publishing a
	// 32-draw prefix immediately before it stranded a one-draw tail in a second
	// GXM batch and reduced r250 from roughly 18.7 to 13.6--14.3 objects/draw.
	// The 120-transaction physical bound still forces an earlier pressure close
	// if a frame does not arrive, so this changes batching granularity, not the
	// maximum private lifetime.
	static constexpr u32 GeneratedLoopKernelAsyncPublicationExecutions = 64;
	alignas(__cachelinesize) std::atomic<u64> m_execute_jobs_enqueued{0};
	alignas(__cachelinesize) std::atomic<u64> m_execute_jobs_completed{0};
	// Worker-private execution state. PCSX2's x86 microVU exits generated
	// code directly and therefore never consults the EE-owned VPU_STAT busy
	// bit while MTVU is active. Vita's block dispatcher needs the equivalent
	// private stop condition without racing the EE core.
	bool m_program_active = false;
	bool m_dt_program_end = false;
	u32 m_pending_program_interrupts = 0;
	// Producer-only counters. Keeping these on the EE side avoids adding an
	// atomic RMW to every VU program merely for hardware telemetry.
	u64 m_profile_execute_enqueues = 0;
	u64 m_profile_wait_calls = 0;
	u64 m_profile_ring_waits = 0;
	u64 m_profile_ring_wait_spins = 0;
	u64 m_profile_compile_barriers = 0;
	u64 m_profile_queue_submissions = 0;
	u64 m_profile_queue_words = 0;
	bool m_micro_write_pending = false;
	u32 m_micro_invalidate_start = 0;
	u32 m_micro_invalidate_end = 0;
	u32 m_gpu_vu_direct_program_token = 0;
	u32 m_gpu_vu_direct_resume_token = 0;
#if defined(VITASX2_GPU_VU_DIRECT_ADMISSION)
	u32 m_gpu_vu_direct_reported_program_token = 0;
	u32 m_gpu_vu_direct_reported_resume_token = 0;
#endif
	u32 m_gpu_vu_direct_program_start_pc = 0;
	u32 m_gpu_vu_direct_program_configuration_bits = 0;
	bool m_gpu_vu_direct_program_prepared = false;
	// Worker-private identity of the authoritative fixed-GXP generation. A
	// nonzero value means VU1's CPU mirror is deliberately stale until a real
	// observer or temporary CPU-provider transition materializes it.
	u64 m_gpu_vu_universal_committed_sequence = 0;
	u32 m_gpu_vu_universal_committed_tpc = 0;
	const u32* m_gpu_vu_universal_committed_vf = nullptr;
	const u32* m_gpu_vu_universal_committed_state = nullptr;
	const u8* m_gpu_vu_universal_committed_memory = nullptr;
	// CPU1 owns these descriptors until both completion consumers have retired
	// them. The GS mailbox sees only stable raw pointers; the immutable replay
	// journal remains here so any transactional GPU rejection can return to the
	// PCSX2 CPU oracle without having published a partial effect.
	struct PendingUniversalGpuVuEpoch
	{
		std::unique_ptr<VitaGpuVu::UniversalGpuVuEpoch> epoch;
		std::vector<VitaGpuVu::VifUnpackSpan> replay_unpacks;
		s32 addr = 0;
		u32 vif_top = 0;
		u32 vif_itop = 0;
		u32 fbrst = 0;
		u32 classified_pairs = 0;
		u64 attempt_started_at = 0;
		bool execution_published = false;
	};
	std::deque<PendingUniversalGpuVuEpoch> m_gpu_vu_universal_pending_epochs;
	std::atomic<u32> m_gpu_vu_universal_pending_epoch_count{0};
	// Generated one-root transactions use the ordinary direct-draw mailbox but
	// retain their complete BUFFER2 successor until GXM vertex completion.
	// CPU1 adopts these in issue order without replaying a guest VU pair.
	std::deque<std::shared_ptr<VitaGpuVu::GeneratedLoopKernelTransaction>>
		m_gpu_vu_generated_pending_transactions;
	// State-only CPU continuations can execute from the private successor while
	// the preceding generated draw is still in flight. They remain ordered
	// behind that GPU sequence and publish only after its transaction commits.
	struct GeneratedPrivateStateLoadContinuation
	{
		static constexpr u32 MemoryWordCount = VU1_MEMSIZE / sizeof(u32);
		static constexpr u32 MemoryMaskWordCount =
			(MemoryWordCount + 31u) / 32u;

		u64 predecessor_sequence = 0;
		u64 sequence = 0;
		std::vector<VitaGpuVu::VifUnpackSpan> replay_unpacks;
		std::array<u32, MemoryMaskWordCount> unavailable_memory_words{};
		std::array<std::array<u32, 4>, 32> vf_values{};
		std::array<u8, 32> vf_lane_masks{};
		std::array<u16, 16> vi_values{};
		u64 expected_memory_fingerprint = 0;
		u64 expected_register_fingerprint = 0;
		u32 vi_write_mask = 0;
			u32 final_tpc = 0;
			u32 final_cycle = 0;
			u32 executed_pairs = 0;
			bool compiled_state_formula = false;
		};
	std::deque<GeneratedPrivateStateLoadContinuation>
		m_gpu_vu_generated_private_continuations;
	u64 m_gpu_vu_private_state_load_queued_count = 0;
	u64 m_gpu_vu_private_state_load_adopted_count = 0;
	u64 m_gpu_vu_generated_batch_drain_count = 0;
	u64 m_gpu_vu_generated_async_publication_count = 0;
	u32 m_gpu_vu_generated_unpublished_transactions = 0;
	bool m_gpu_vu_private_state_bridge_quarantined = false;
	// Counts both GPU transactions and ordered private continuations so the EE
	// execution budget cannot outrun guest-visible completion.
	std::atomic<u32> m_gpu_vu_generated_pending_transaction_count{0};
	// CPU1-private successor used only to construct a later independent GPU
	// transaction while an earlier BUFFER2 journal is still in flight.  It is
	// never guest-visible or copied back to canonical VU1 state.  Native SGX
	// output-only lanes remain explicitly unavailable until a later full-qword
	// UNPACK overwrites them or the owning transaction retires.
		struct GeneratedLoopKernelPrivateState
		{
		static constexpr u32 MemoryWordCount = VU1_MEMSIZE / sizeof(u32);
		static constexpr u32 MemoryMaskWordCount =
			(MemoryWordCount + 31u) / 32u;
		std::array<u32, MemoryWordCount> memory{};
		// Qword-granular provenance for the permanently mapped canonical VU1
		// region. A bit remains set only while no deferred UNPACK, VU store, or
		// generated transaction has replaced that qword in this private
		// generation. Value equality is deliberately insufficient ownership.
			std::array<u32, (VU1_MEMSIZE / 16u + 31u) / 32u>
				canonical_memory_qwords{};
			// Immutable payload ownership survives Execute retirement. Generated
			// descriptors can therefore consume the original mapped VIF bytes rather
			// than repacking the materialized CPU shadow on every invocation.
			std::shared_ptr<VitaGpuVu::PersistentVifMemoryProvenance>
				raw_memory_provenance;
		std::array<u32, MemoryMaskWordCount>
			unavailable_memory_words{};
		// Slot-backed owners keep their current sequence in one compact record.
		// The per-word value remains the fallback for uncommon shapes which
		// exhaust the fixed slot set; UnavailableMemoryOwnerSequence() is the
		// only supported reader.
		std::array<u64, MemoryWordCount>
			unavailable_memory_owner_sequence{};
		// Values 1..64 index deferred_memory_owners. Values with bit 7 set index
		// native_memory_owners. Zero retains the direct per-word fallback above.
		std::array<u8, MemoryWordCount> unavailable_memory_owner_slot{};
		static constexpr u8 NativeMemoryOwnerSlotBit = 0x80u;
		static constexpr u32 NativeMemoryOwnerCapacity = 8u;
                struct NativeMemoryOwner {
                  u64 sequence = 0u;
                  const VitaGpuVu::GeneratedLoopKernelTransactionLayout
                      *layout_identity = nullptr;
                  std::array<u32, MemoryMaskWordCount> live_word_masks{};
                  u32 live_word_count = 0u;
                  u32 full_layout_word_count = 0u;
                  // Vertex completion copied every still-owned BUFFER2 lane
                  // into the private memory image. Keep the lane unavailable
                  // until an actual observer so a following generated
                  // transaction can replace this complete ownership partition
                  // without rescattering it on CPU1.
                  bool materialized = false;
                };
                std::array<NativeMemoryOwner, NativeMemoryOwnerCapacity>
			native_memory_owners{};
		struct DeferredMemoryOwner
		{
			u64 sequence = 0;
			std::shared_ptr<const VitaGpuVu::GeneratedLoopKernelDeferredStores>
				owner;
			const VitaGpuVu::GeneratedLoopKernelTransactionLayout*
				layout_identity = nullptr;
			// Exact lanes which still resolve through this slot.  Keeping the
			// compact 512-byte mask with the retained value graph lets a later
			// transaction prove that it supersedes several differently sized
			// layouts without rescattering every lane on CPU1.
			std::array<u32, MemoryMaskWordCount> live_word_masks{};
			u32 live_word_count = 0;
			u32 full_layout_word_count = 0;
		};
		static constexpr u32 DeferredMemoryOwnerCapacity = 64u;
		std::vector<DeferredMemoryOwner> deferred_memory_owners;

		u32 LiveDeferredMemoryOwnerCount() const
		{
			return static_cast<u32>(std::count_if(
				deferred_memory_owners.begin(), deferred_memory_owners.end(),
				[](const DeferredMemoryOwner& retained) {
					return retained.live_word_count != 0u;
				}));
		}

		u8 RetainDeferredMemoryOwner(
			u64 sequence,
			std::shared_ptr<const VitaGpuVu::GeneratedLoopKernelDeferredStores>
				owner,
			const VitaGpuVu::GeneratedLoopKernelTransactionLayout* layout_identity,
			u32 full_layout_word_count)
		{
			if (sequence == 0u || !owner || !layout_identity ||
				full_layout_word_count == 0u)
				return 0u;
			for (u32 index = 0u; index < deferred_memory_owners.size(); index++)
			{
				DeferredMemoryOwner& retained = deferred_memory_owners[index];
				if (retained.sequence == sequence)
					return 0u;
				if (retained.live_word_count != 0u)
					continue;
				retained.sequence = sequence;
				retained.owner = std::move(owner);
				retained.layout_identity = layout_identity;
				retained.live_word_masks.fill(0u);
				retained.full_layout_word_count = full_layout_word_count;
				return static_cast<u8>(index + 1u);
			}
			if (deferred_memory_owners.size() >= DeferredMemoryOwnerCapacity)
				return 0u;
			deferred_memory_owners.emplace_back();
			DeferredMemoryOwner& retained = deferred_memory_owners.back();
			retained.sequence = sequence;
			retained.owner = std::move(owner);
			retained.layout_identity = layout_identity;
			retained.full_layout_word_count = full_layout_word_count;
			return static_cast<u8>(deferred_memory_owners.size());
		}

		// A generated chain commonly alternates one full grid and one smaller
		// prefix grid.  Both are exact PairPlan-derived transactions, but their
		// immutable layouts differ, so exact-identity replacement alone falls
		// back to a lane-by-lane owner scatter.  Prove instead that every lane
		// written by the new transaction is already private and that each live
		// owner it intersects is wholly covered.  The existing slot map can then
		// remain unchanged while all covered slots adopt the new value graph.
		// Any available lane, native-output slot, or partial owner overlap rejects
		// this fast path before mutation.
		bool ReplaceCoveredDeferredMemoryOwners(
			u64 sequence,
			std::shared_ptr<const VitaGpuVu::GeneratedLoopKernelDeferredStores>
				owner,
			const std::array<u32, MemoryMaskWordCount>& store_word_masks,
			u32* replaced_owner_count)
		{
			if (replaced_owner_count)
				*replaced_owner_count = 0u;
			if (sequence == 0u || !owner)
				return false;

			std::array<u32, MemoryMaskWordCount> covered{};
			u32 replacements = 0u;
			for (u32 mask_word = 0u; mask_word < store_word_masks.size();
				mask_word++)
			{
				// A lane without a deferred owner would require a new slot mapping.
				// Leave that mixed case to SetUnavailableMemoryOwnerMask().
				if ((store_word_masks[mask_word] &
					~unavailable_memory_words[mask_word]) != 0u)
				{
					return false;
				}
			}

			for (const DeferredMemoryOwner& retained : deferred_memory_owners)
			{
				if (retained.live_word_count == 0u)
					continue;
				bool intersects = false;
				bool wholly_covered = true;
				for (u32 mask_word = 0u; mask_word < store_word_masks.size();
					mask_word++)
				{
					const u32 live = retained.live_word_masks[mask_word];
					intersects |= (live & store_word_masks[mask_word]) != 0u;
					wholly_covered &=
						(live & ~store_word_masks[mask_word]) == 0u;
				}
				if (!intersects)
					continue;
				if (!wholly_covered)
					return false;
				for (u32 mask_word = 0u; mask_word < covered.size(); mask_word++)
					covered[mask_word] |= retained.live_word_masks[mask_word];
				replacements++;
			}

			if (replacements == 0u || covered != store_word_masks)
				return false;

			for (DeferredMemoryOwner& retained : deferred_memory_owners)
			{
				if (retained.live_word_count == 0u)
					continue;
				bool covered_owner = false;
				for (u32 mask_word = 0u; mask_word < store_word_masks.size();
					mask_word++)
				{
					covered_owner |= retained.live_word_masks[mask_word] != 0u &&
						(retained.live_word_masks[mask_word] &
						 store_word_masks[mask_word]) != 0u;
				}
				if (!covered_owner)
					continue;
				retained.sequence = sequence;
				retained.owner = owner;
			}
			if (replaced_owner_count)
				*replaced_owner_count = replacements;
			return true;
		}

		// Exact same-layout output generations are a last-writer-wins chain.
		// When every lane of the prior generation is still live, no UNPACK,
		// partial store, or observer has touched it. Replace the retained value
		// graph in one operation and leave the direct lane-to-slot map intact.
		u8 ReplaceFullyLiveDeferredMemoryOwner(
			u64 sequence,
			std::shared_ptr<const VitaGpuVu::GeneratedLoopKernelDeferredStores>
				owner,
			const VitaGpuVu::GeneratedLoopKernelTransactionLayout* layout_identity,
			u32 full_layout_word_count)
		{
			if (sequence == 0u || !owner || !layout_identity ||
				full_layout_word_count == 0u)
				return 0u;
			for (u32 index = 0u; index < deferred_memory_owners.size(); index++)
			{
				DeferredMemoryOwner& retained = deferred_memory_owners[index];
				if (retained.layout_identity != layout_identity ||
					retained.full_layout_word_count != full_layout_word_count ||
					retained.live_word_count != full_layout_word_count)
				{
					continue;
				}
				retained.sequence = sequence;
				retained.owner = std::move(owner);
				return static_cast<u8>(index + 1u);
			}
			return 0u;
		}

		static bool IsNativeMemoryOwnerSlot(u8 slot)
		{
			return (slot & NativeMemoryOwnerSlotBit) != 0u;
		}

		static u32 NativeMemoryOwnerIndex(u8 slot)
		{
			return static_cast<u32>((slot & ~NativeMemoryOwnerSlotBit) - 1u);
		}

		u64 UnavailableMemoryOwnerSequence(u32 word) const
		{
			const u8 slot = unavailable_memory_owner_slot[word];
			if (!IsNativeMemoryOwnerSlot(slot))
				return unavailable_memory_owner_sequence[word];
			const u32 owner_index = NativeMemoryOwnerIndex(slot);
			pxAssertRel(owner_index < native_memory_owners.size(),
				"native GPU-VU memory owner slot is outside its pool");
			return owner_index < native_memory_owners.size() ?
				native_memory_owners[owner_index].sequence : 0u;
		}

		u8 RetainNativeMemoryOwner(
			u64 sequence,
			const VitaGpuVu::GeneratedLoopKernelTransactionLayout* layout_identity,
			u32 full_layout_word_count)
		{
			if (sequence == 0u || !layout_identity ||
				full_layout_word_count == 0u)
				return 0u;
			for (u32 index = 0u; index < native_memory_owners.size(); index++)
			{
				NativeMemoryOwner& retained = native_memory_owners[index];
				if (retained.live_word_count != 0u)
					continue;
				retained.sequence = sequence;
				retained.layout_identity = layout_identity;
				retained.live_word_masks.fill(0u);
				retained.full_layout_word_count = full_layout_word_count;
				retained.materialized = false;
				return static_cast<u8>(NativeMemoryOwnerSlotBit | (index + 1u));
			}
			return 0u;
		}

                u8 ReplaceFullyLiveNativeMemoryOwner(
                    u64 sequence,
                    const VitaGpuVu::GeneratedLoopKernelTransactionLayout
                        *layout_identity,
                    u32 full_layout_word_count) {
                  if (sequence == 0u || !layout_identity ||
                      full_layout_word_count == 0u)
                    return 0u;
                  for (u32 index = 0u; index < native_memory_owners.size();
                       index++) {
                    NativeMemoryOwner &retained = native_memory_owners[index];
                    if (retained.layout_identity != layout_identity ||
                        retained.full_layout_word_count !=
                            full_layout_word_count ||
                        retained.live_word_count != full_layout_word_count) {
                      continue;
                    }
                    retained.sequence = sequence;
                    retained.materialized = false;
                    return static_cast<u8>(NativeMemoryOwnerSlotBit |
                                           (index + 1u));
                  }
                  return 0u;
                }

                bool ReplaceCoveredNativeMemoryOwners(
                    u64 sequence,
                    const VitaGpuVu::GeneratedLoopKernelTransactionLayout
                        *layout_identity,
                    u32 full_layout_word_count,
                    const std::array<u32, MemoryMaskWordCount>
                        &store_word_masks,
                    u32 *replaced_owner_count) {
                  if (replaced_owner_count)
                    *replaced_owner_count = 0u;
                  if (sequence == 0u || !layout_identity ||
                      full_layout_word_count == 0u)
                    return false;

                  for (u32 mask_word = 0u; mask_word < store_word_masks.size();
                       mask_word++) {
                    if ((store_word_masks[mask_word] &
                         ~unavailable_memory_words[mask_word]) != 0u) {
                      return false;
                    }
                  }

                  std::array<u32, MemoryMaskWordCount> covered{};
                  std::array<bool, NativeMemoryOwnerCapacity> replacements{};
                  u32 replacement_count = 0u;
                  for (u32 index = 0u; index < native_memory_owners.size();
                       index++) {
                    const NativeMemoryOwner &retained =
                        native_memory_owners[index];
                    if (retained.live_word_count == 0u)
                      continue;
                    bool intersects = false;
                    bool wholly_covered = true;
                    for (u32 mask_word = 0u;
                         mask_word < store_word_masks.size(); mask_word++) {
                      const u32 live = retained.live_word_masks[mask_word];
                      intersects |= (live & store_word_masks[mask_word]) != 0u;
                      wholly_covered &=
                          (live & ~store_word_masks[mask_word]) == 0u;
                    }
                    if (!intersects)
                      continue;
                    if (!wholly_covered)
                      return false;
                    for (u32 mask_word = 0u; mask_word < covered.size();
                         mask_word++)
                      covered[mask_word] |= retained.live_word_masks[mask_word];
                    replacements[index] = true;
                    replacement_count++;
                  }
                  if (replacement_count == 0u || covered != store_word_masks)
                    return false;

                  for (u32 index = 0u; index < native_memory_owners.size();
                       index++) {
                    if (!replacements[index])
                      continue;
                    NativeMemoryOwner &retained = native_memory_owners[index];
                    retained.sequence = sequence;
                    retained.materialized = false;
                    // Preserve the partition's own shape. A large transaction
                    // can cover
                    // both the small-prefix and remainder slots; retaining the
                    // prefix identity lets the next alternating small
                    // transaction replace that slot in O(1) instead of
                    // rescanning all memory masks.
                  }
                  if (replaced_owner_count)
                    *replaced_owner_count = replacement_count;
                  return true;
                }

                u32 LiveNativeMemoryOwnerWordCount(u64 sequence) const {
                  u32 words = 0u;
                  for (const NativeMemoryOwner &retained :
                       native_memory_owners) {
                    if (retained.sequence == sequence)
                      words += retained.live_word_count;
                  }
                  return words;
                }

                u32 LiveFallbackMemoryOwnerWordCount(u64 sequence) const {
                  u32 words = 0u;
                  for (u32 word = 0u;
                       word < unavailable_memory_owner_slot.size(); word++) {
                    if (unavailable_memory_owner_slot[word] == 0u &&
                        unavailable_memory_owner_sequence[word] == sequence) {
                      words++;
                    }
                  }
                  return words;
                }

                bool MarkNativeMemoryOwnerMaterialized(u64 sequence) {
                  bool found = false;
                  for (NativeMemoryOwner &retained : native_memory_owners) {
                    if (retained.sequence != sequence ||
                        retained.live_word_count == 0u)
                      continue;
                    retained.materialized = true;
                    found = true;
                  }
                  return found;
                }

                // A real CPU/VIF observer runs only after every physical
                // transaction has retired. At that boundary the latest native
                // owners already have their values in memory[], so publishing
                // them means clearing metadata only. Validate all owners before
                // mutation to keep observer failure atomic.
                bool PublishMaterializedNativeMemoryOwners() {
                  for (u32 owner_index = 0u;
                       owner_index < native_memory_owners.size();
                       owner_index++) {
                    const NativeMemoryOwner &retained =
                        native_memory_owners[owner_index];
                    if (retained.live_word_count != 0u &&
                        !retained.materialized)
                      return false;
                    if (retained.live_word_count == 0u)
                      continue;
                    const u8 owner_slot = static_cast<u8>(
                        NativeMemoryOwnerSlotBit | (owner_index + 1u));
                    u32 owned_words = 0u;
                    for (u32 mask_word = 0u;
                         mask_word < retained.live_word_masks.size();
                         mask_word++) {
                      u32 pending = retained.live_word_masks[mask_word];
                      owned_words +=
                          static_cast<u32>(__builtin_popcount(pending));
                      while (pending != 0u) {
                        const u32 bit =
                            static_cast<u32>(__builtin_ctz(pending));
                        const u32 word = mask_word * 32u + bit;
                        if (unavailable_memory_owner_slot[word] != owner_slot ||
                            (unavailable_memory_words[mask_word] &
                             (1u << bit)) == 0u) {
                          return false;
                        }
                        pending &= pending - 1u;
                      }
                    }
                    if (owned_words != retained.live_word_count)
                      return false;
                  }
                  for (u32 owner_index = 0u;
                       owner_index < native_memory_owners.size();
                       owner_index++) {
                    NativeMemoryOwner &retained =
                        native_memory_owners[owner_index];
                    if (retained.live_word_count == 0u)
                      continue;
                    const u8 owner_slot = static_cast<u8>(
                        NativeMemoryOwnerSlotBit | (owner_index + 1u));
                    for (u32 mask_word = 0u;
                         mask_word < retained.live_word_masks.size();
                         mask_word++) {
                      u32 pending = retained.live_word_masks[mask_word];
                      unavailable_memory_words[mask_word] &= ~pending;
                      while (pending != 0u) {
                        const u32 bit =
                            static_cast<u32>(__builtin_ctz(pending));
                        const u32 word = mask_word * 32u + bit;
                        pxAssertRel(unavailable_memory_owner_slot[word] ==
                                        owner_slot,
                                    "validated native GPU-VU owner slot "
                                    "changed during publication");
                        unavailable_memory_owner_slot[word] = 0u;
                        unavailable_memory_owner_sequence[word] = 0u;
                        pending &= pending - 1u;
                      }
                    }
                    retained = {};
                  }
                  return true;
                }

		// A direct V4-32 UNPACK replaces complete consecutive qwords.  Preserve
		// that command-level shape here instead of expanding it into four calls
		// per vector.  Only lanes which actually have a deferred/generated owner
		// need slot bookkeeping; ordinary available lanes are cleared a mask word
		// at a time.  Wrapping at the architectural 16 KiB VU1 boundary is exact.
		static void MarkCompleteQwordSpanAvailable(
			std::array<u32, MemoryMaskWordCount>* unavailable,
			u16 first_qword, u32 qword_count)
		{
			if (!unavailable || qword_count == 0u)
				return;
			const auto clear_linear_bits = [](auto* masks, u32 first_bit,
					u32 bit_count) {
				while (bit_count != 0u)
				{
					const u32 mask_word = first_bit >> 5u;
					const u32 offset = first_bit & 31u;
					const u32 take = std::min(bit_count, 32u - offset);
					const u32 low_bits = take == 32u ? ~0u :
						((1u << take) - 1u);
					(*masks)[mask_word] &= ~(low_bits << offset);
					first_bit += take;
					bit_count -= take;
				}
			};
			u32 qword = static_cast<u32>(first_qword) & 0x3ffu;
			while (qword_count != 0u)
			{
				const u32 chunk = std::min(qword_count, 1024u - qword);
				clear_linear_bits(unavailable, qword * 4u, chunk * 4u);
				qword_count -= chunk;
				qword = 0u;
			}
		}

		void ApplyCompleteQwordUnpackOwnership(
			u16 first_qword, u32 qword_count)
		{
			if (qword_count == 0u)
				return;
			const auto bit_range_mask = [](u32 offset, u32 count) {
				const u32 low_bits = count == 32u ? ~0u :
					((1u << count) - 1u);
				return low_bits << offset;
			};
			u32 qword = static_cast<u32>(first_qword) & 0x3ffu;
			while (qword_count != 0u)
			{
				const u32 chunk = std::min(qword_count, 1024u - qword);

				u32 canonical_first = qword;
				u32 canonical_count = chunk;
				while (canonical_count != 0u)
				{
					const u32 mask_word = canonical_first >> 5u;
					const u32 offset = canonical_first & 31u;
					const u32 take = std::min(
						canonical_count, 32u - offset);
					canonical_memory_qwords[mask_word] &=
						~bit_range_mask(offset, take);
					canonical_first += take;
					canonical_count -= take;
				}

				u32 first_word = qword * 4u;
				u32 word_count = chunk * 4u;
				while (word_count != 0u)
				{
					const u32 mask_word = first_word >> 5u;
					const u32 offset = first_word & 31u;
					const u32 take = std::min(word_count, 32u - offset);
					const u32 range = bit_range_mask(offset, take);
					u32 pending = unavailable_memory_words[mask_word] & range;
					while (pending != 0u)
					{
						const u32 bit = static_cast<u32>(__builtin_ctz(pending));
						ClearUnavailableMemoryOwner(mask_word * 32u + bit);
						pending &= pending - 1u;
					}
					unavailable_memory_words[mask_word] &= ~range;
					first_word += take;
					word_count -= take;
				}

				qword_count -= chunk;
				qword = 0u;
			}
		}

                void ClearUnavailableMemoryOwner(u32 word) {
                  const u64 prior_sequence =
                      unavailable_memory_owner_sequence[word];
                  const u8 prior_slot = unavailable_memory_owner_slot[word];
                  if (prior_sequence == 0u && prior_slot == 0u)
                    return;
                  if (IsNativeMemoryOwnerSlot(prior_slot)) {
                    const u32 owner_index = NativeMemoryOwnerIndex(prior_slot);
                    pxAssertRel(
                        owner_index < native_memory_owners.size(),
                        "native GPU-VU memory owner slot is outside its pool");
                    if (owner_index < native_memory_owners.size()) {
                      NativeMemoryOwner &retained =
                          native_memory_owners[owner_index];
                      pxAssertRel(
                          retained.live_word_count != 0u,
                          "native GPU-VU memory owner lost its direct slot");
                      if (retained.live_word_count != 0u) {
                        retained.live_word_count--;
                        retained.live_word_masks[word >> 5] &=
                            ~(1u << (word & 31u));
                      }
                    }
                  } else if (prior_slot != 0u) {
                    const u32 owner_index = static_cast<u32>(prior_slot - 1u);
                    pxAssertRel(owner_index < deferred_memory_owners.size(),
                                "deferred GPU-VU memory owner slot is outside "
                                "its pool");
                    if (owner_index < deferred_memory_owners.size()) {
                      DeferredMemoryOwner &retained =
                          deferred_memory_owners[owner_index];
                      pxAssertRel(
                          retained.live_word_count != 0u,
                          "deferred GPU-VU memory owner lost its direct slot");
                      if (retained.live_word_count != 0u) {
                        retained.live_word_count--;
                        retained.live_word_masks[word >> 5] &=
                            ~(1u << (word & 31u));
                      }
                    }
                  }
                  unavailable_memory_owner_sequence[word] = 0u;
                  unavailable_memory_owner_slot[word] = 0u;
                }

                void SetUnavailableMemoryOwner(u32 word, u64 sequence, u8 owner_slot)
		{
			if (UnavailableMemoryOwnerSequence(word) == sequence &&
				unavailable_memory_owner_slot[word] == owner_slot)
				return;
			ClearUnavailableMemoryOwner(word);
			unavailable_memory_owner_sequence[word] = sequence;
			unavailable_memory_owner_slot[word] = owner_slot;
			if (IsNativeMemoryOwnerSlot(owner_slot))
			{
				const u32 owner_index = NativeMemoryOwnerIndex(owner_slot);
				pxAssertRel(owner_index < native_memory_owners.size() &&
					native_memory_owners[owner_index].sequence == sequence,
					"native GPU-VU memory owner slot changed before assignment");
				if (owner_index < native_memory_owners.size() &&
					native_memory_owners[owner_index].sequence == sequence)
				{
					native_memory_owners[owner_index].live_word_count++;
					native_memory_owners[owner_index].
						live_word_masks[word >> 5] |= 1u << (word & 31u);
				}
			}
			else if (owner_slot != 0u)
			{
				const u32 owner_index = static_cast<u32>(owner_slot - 1u);
				pxAssertRel(owner_index < deferred_memory_owners.size() &&
					deferred_memory_owners[owner_index].sequence == sequence,
					"deferred GPU-VU memory owner slot changed before assignment");
				if (owner_index < deferred_memory_owners.size() &&
					deferred_memory_owners[owner_index].sequence == sequence)
				{
					deferred_memory_owners[owner_index].live_word_count++;
					deferred_memory_owners[owner_index].
						live_word_masks[word >> 5] |= 1u << (word & 31u);
				}
			}
		}

		void SetUnavailableMemoryOwnerMask(
			const std::array<u32, MemoryMaskWordCount>& masks,
			u64 sequence, u8 owner_slot)
		{
			for (u32 mask_word = 0u; mask_word < masks.size(); mask_word++)
			{
				u32 pending = masks[mask_word];
				unavailable_memory_words[mask_word] |= pending;
				while (pending != 0u)
				{
					const u32 bit = static_cast<u32>(__builtin_ctz(pending));
					SetUnavailableMemoryOwner(
						mask_word * 32u + bit, sequence, owner_slot);
					pending &= pending - 1u;
				}
			}
		}

		void ReleaseDeadDeferredMemoryOwners()
		{
			for (NativeMemoryOwner& retained : native_memory_owners)
			{
				if (retained.live_word_count == 0u)
					retained = {};
			}
			for (DeferredMemoryOwner& retained : deferred_memory_owners)
			{
				if (retained.live_word_count != 0u)
					continue;
				retained.sequence = 0u;
				retained.owner.reset();
				retained.layout_identity = nullptr;
				retained.live_word_masks.fill(0u);
				retained.full_layout_word_count = 0u;
			}
		}

		// Register successors use the same transactional last-writer ownership as
		// deferred VU memory, but the old representation retained one shared_ptr
		// in every VF/ACC/scalar lane. A complete BSpline successor commonly writes
		// nearly the whole register file, turning one logical owner update into more
		// than one hundred atomic reference-count operations on CPU1. Keep each
		// owner once and map lanes to it with compact slot IDs instead.
		static constexpr u32 DeferredRegisterOwnerCapacity = 16u;
		struct DeferredRegisterOwner
		{
			std::shared_ptr<const
				VitaGpuVu::GeneratedLoopKernelDeferredSuccessor> owner;
			u32 live_reference_count = 0u;
		};
		std::array<DeferredRegisterOwner, DeferredRegisterOwnerCapacity>
			deferred_register_owners{};
		std::array<u8, 32u * 4u> deferred_vf_owner_slots{};
		std::array<u8, 4u> deferred_acc_owner_slots{};
		u8 deferred_q_owner_slot = 0u;
		u8 deferred_p_owner_slot = 0u;
		u8 deferred_i_owner_slot = 0u;

		u8 FindDeferredRegisterOwner(
			const std::shared_ptr<const
				VitaGpuVu::GeneratedLoopKernelDeferredSuccessor>& owner) const
		{
			if (!owner)
				return 0u;
			for (u32 index = 0u; index < deferred_register_owners.size(); index++)
			{
				const DeferredRegisterOwner& retained =
					deferred_register_owners[index];
				if (retained.live_reference_count != 0u &&
					retained.owner.get() == owner.get())
				{
					return static_cast<u8>(index + 1u);
				}
			}
			return 0u;
		}

		const std::shared_ptr<const
			VitaGpuVu::GeneratedLoopKernelDeferredSuccessor>&
		DeferredRegisterOwnerForSlot(u8 slot) const
		{
			static const std::shared_ptr<const
				VitaGpuVu::GeneratedLoopKernelDeferredSuccessor> empty;
			if (slot == 0u || slot > deferred_register_owners.size())
				return empty;
			return deferred_register_owners[slot - 1u].owner;
		}

		void ClearDeferredRegisterOwnerSlot(u8* slot)
		{
			if (!slot || *slot == 0u)
				return;
			const u32 index = static_cast<u32>(*slot - 1u);
			pxAssertRel(index < deferred_register_owners.size(),
				"deferred GPU-VU register owner slot is outside its pool");
			if (index < deferred_register_owners.size())
			{
				DeferredRegisterOwner& retained = deferred_register_owners[index];
				pxAssertRel(retained.live_reference_count != 0u && retained.owner,
					"deferred GPU-VU register owner lost its direct slot");
				if (retained.live_reference_count != 0u)
				{
					retained.live_reference_count--;
					if (retained.live_reference_count == 0u)
						retained.owner.reset();
				}
			}
			*slot = 0u;
		}

		u8 RetainDeferredRegisterOwner(
			std::shared_ptr<const
				VitaGpuVu::GeneratedLoopKernelDeferredSuccessor> owner)
		{
			if (!owner)
				return 0u;
			if (const u8 existing = FindDeferredRegisterOwner(owner); existing != 0u)
				return existing;
			for (u32 index = 0u; index < deferred_register_owners.size(); index++)
			{
				DeferredRegisterOwner& retained = deferred_register_owners[index];
				if (retained.live_reference_count != 0u)
					continue;
				retained.owner = std::move(owner);
				return static_cast<u8>(index + 1u);
			}
			return 0u;
		}

		void SetDeferredRegisterOwnerSlot(u8* destination, u8 owner_slot)
		{
			if (!destination || owner_slot == 0u ||
				owner_slot > deferred_register_owners.size())
			{
				return;
			}
			if (*destination == owner_slot)
				return;
			ClearDeferredRegisterOwnerSlot(destination);
			DeferredRegisterOwner& retained =
				deferred_register_owners[owner_slot - 1u];
			pxAssertRel(retained.owner,
				"deferred GPU-VU register owner slot was not retained");
			if (!retained.owner)
				return;
			*destination = owner_slot;
			retained.live_reference_count++;
		}

		template <typename Visitor>
		void VisitDeferredRegisterDestinations(
			const std::array<u8, 32>& vf_lanes, u8 acc_lanes,
			bool final_q, bool final_p, bool final_i, Visitor&& visitor)
		{
			for (u32 reg = 1u; reg < vf_lanes.size(); reg++)
			{
				for (u32 lane = 0u; lane < 4u; lane++)
				{
					if ((vf_lanes[reg] & (0x8u >> lane)) != 0u)
						visitor(&deferred_vf_owner_slots[reg * 4u + lane]);
				}
			}
			for (u32 lane = 0u; lane < 4u; lane++)
			{
				if ((acc_lanes & (0x8u >> lane)) != 0u)
					visitor(&deferred_acc_owner_slots[lane]);
			}
			if (final_q)
				visitor(&deferred_q_owner_slot);
			if (final_p)
				visitor(&deferred_p_owner_slot);
			if (final_i)
				visitor(&deferred_i_owner_slot);
		}

		bool CanAssignDeferredRegisterOwner(
			const std::shared_ptr<const
				VitaGpuVu::GeneratedLoopKernelDeferredSuccessor>& owner,
			const std::array<u8, 32>& vf_lanes, u8 acc_lanes,
			bool final_q, bool final_p, bool final_i) const
		{
			if (!owner)
				return false;
			if (FindDeferredRegisterOwner(owner) != 0u ||
				std::any_of(deferred_register_owners.begin(),
					deferred_register_owners.end(), [](const auto& retained) {
						return retained.live_reference_count == 0u;
					}))
			{
				return true;
			}

			// A full pool is still admissible when this transaction completely
			// supersedes at least one old owner. Its destination slots are cleared
			// before retaining the new owner, making that slot available without an
			// observer or allocation.
			std::array<u32, DeferredRegisterOwnerCapacity> covered{};
			const auto count_slot = [&covered](u8 slot) {
				if (slot != 0u && slot <= covered.size())
					covered[slot - 1u]++;
			};
			for (u32 reg = 1u; reg < vf_lanes.size(); reg++)
			{
				for (u32 lane = 0u; lane < 4u; lane++)
				{
					if ((vf_lanes[reg] & (0x8u >> lane)) != 0u)
						count_slot(deferred_vf_owner_slots[reg * 4u + lane]);
				}
			}
			for (u32 lane = 0u; lane < 4u; lane++)
			{
				if ((acc_lanes & (0x8u >> lane)) != 0u)
					count_slot(deferred_acc_owner_slots[lane]);
			}
			if (final_q)
				count_slot(deferred_q_owner_slot);
			if (final_p)
				count_slot(deferred_p_owner_slot);
			if (final_i)
				count_slot(deferred_i_owner_slot);
			for (u32 index = 0u; index < deferred_register_owners.size(); index++)
			{
				if (covered[index] != 0u &&
					covered[index] ==
						deferred_register_owners[index].live_reference_count)
				{
					return true;
				}
			}
			return false;
		}

		bool ReplaceCoveredDeferredRegisterOwners(
			const std::shared_ptr<const
				VitaGpuVu::GeneratedLoopKernelDeferredSuccessor>& owner,
			const std::array<u8, 32>& vf_lanes, u8 acc_lanes,
			bool final_q, bool final_p, bool final_i,
			u32* replaced_owner_slots = nullptr)
		{
			if (replaced_owner_slots)
				*replaced_owner_slots = 0u;
			if (!owner)
				return false;
			std::array<u32, DeferredRegisterOwnerCapacity> covered{};
			u32 destination_count = 0u;
			bool valid = true;
			const auto count_slot = [&covered, &destination_count, &valid](
					u8 slot, bool unavailable) {
				if (!unavailable || slot == 0u || slot > covered.size())
				{
					valid = false;
					return;
				}
				covered[slot - 1u]++;
				destination_count++;
			};
			for (u32 reg = 1u; reg < vf_lanes.size(); reg++)
			{
				for (u32 lane = 0u; lane < 4u; lane++)
				{
					const u8 bit = static_cast<u8>(0x8u >> lane);
					if ((vf_lanes[reg] & bit) == 0u)
						continue;
					count_slot(deferred_vf_owner_slots[reg * 4u + lane],
						(unavailable_vf_lanes[reg] & bit) != 0u);
				}
			}
			for (u32 lane = 0u; lane < 4u; lane++)
			{
				const u8 bit = static_cast<u8>(0x8u >> lane);
				if ((acc_lanes & bit) != 0u)
				{
					count_slot(deferred_acc_owner_slots[lane],
						(unavailable_acc_lanes & bit) != 0u);
				}
			}
			if (final_q)
				count_slot(deferred_q_owner_slot, !q_available);
			if (final_p)
				count_slot(deferred_p_owner_slot, !p_available);
			if (final_i)
				count_slot(deferred_i_owner_slot, !i_available);
			if (!valid || destination_count == 0u)
				return false;
			for (u32 index = 0u; index < deferred_register_owners.size(); index++)
			{
				if (covered[index] != 0u &&
					covered[index] !=
						deferred_register_owners[index].live_reference_count)
				{
					return false;
				}
			}
			for (u32 index = 0u; index < deferred_register_owners.size(); index++)
			{
				if (covered[index] != 0u)
					deferred_register_owners[index].owner = owner;
			}
			if (replaced_owner_slots)
			{
				*replaced_owner_slots = static_cast<u32>(std::count_if(
					covered.begin(), covered.end(),
					[](u32 references) { return references != 0u; }));
			}
			return true;
		}

		bool AssignDeferredRegisterOwner(
			std::shared_ptr<const
				VitaGpuVu::GeneratedLoopKernelDeferredSuccessor> owner,
			const std::array<u8, 32>& vf_lanes, u8 acc_lanes,
			bool final_q, bool final_p, bool final_i,
			bool* replaced_owner = nullptr,
			u32* replaced_owner_slots = nullptr)
		{
			if (replaced_owner)
				*replaced_owner = false;
			if (replaced_owner_slots)
				*replaced_owner_slots = 0u;
			if (!owner)
				return false;
			u32 replacement_count = 0u;
			if (ReplaceCoveredDeferredRegisterOwners(
					owner, vf_lanes, acc_lanes, final_q, final_p, final_i,
					&replacement_count))
			{
				if (replaced_owner)
					*replaced_owner = true;
				if (replaced_owner_slots)
					*replaced_owner_slots = replacement_count;
				return true;
			}
			VisitDeferredRegisterDestinations(
				vf_lanes, acc_lanes, final_q, final_p, final_i,
				[this](u8* slot) { ClearDeferredRegisterOwnerSlot(slot); });
			const u8 owner_slot = RetainDeferredRegisterOwner(std::move(owner));
			if (owner_slot == 0u)
				return false;
			VisitDeferredRegisterDestinations(
				vf_lanes, acc_lanes, final_q, final_p, final_i,
				[this, owner_slot](u8* slot) {
					SetDeferredRegisterOwnerSlot(slot, owner_slot);
				});
			return true;
		}

		void ResetDeferredRegisterOwners()
		{
			deferred_register_owners = {};
			deferred_vf_owner_slots.fill(0u);
			deferred_acc_owner_slots.fill(0u);
			deferred_q_owner_slot = 0u;
			deferred_p_owner_slot = 0u;
			deferred_i_owner_slot = 0u;
		}

			// Reused by real architectural observers. This must match the bounded
			// product journal class asserted in VU_Thread's constructor; keeping the
			// evaluator scratch here removes observer-time newlib allocation and
			// fragmentation without retaining the rejected 8,192-word overprovision.
			static constexpr u32 DeferredStoreWordCapacity = 2048u;
		std::array<u32, DeferredStoreWordCapacity> deferred_store_words{};
		std::array<std::array<u32, 4>, 32> vf{};
		std::array<u32, 4> acc{};
		std::array<u8, 32> unavailable_vf_lanes{};
		u8 unavailable_acc_lanes = 0;
		std::array<u16, 16> vi{};
		u32 q = 0;
		u32 p = 0;
		u32 i = 0;
		u32 tpc = 0;
		bool q_available = true;
		bool p_available = true;
		bool i_available = true;
		bool valid = false;
	};
	struct GeneratedLoopKernelPrivateAdvanceProof
	{
		const VitaGpuVu::GeneratedLoopKernelTransaction* transaction = nullptr;
		const VitaGpuVu::VifUnpackSpan* unpack_data = nullptr;
		size_t unpack_count = 0u;

		bool Matches(
			const VitaGpuVu::GeneratedLoopKernelTransaction& candidate_transaction,
			const std::vector<VitaGpuVu::VifUnpackSpan>& candidate_unpacks) const
		{
			return transaction == &candidate_transaction &&
				unpack_data == candidate_unpacks.data() &&
				unpack_count == candidate_unpacks.size();
		}
	};
	GeneratedLoopKernelPrivateState m_gpu_vu_generated_private_state;
	u64 m_gpu_vu_generated_private_tail_sequence = 0;
	u64 m_gpu_vu_generated_private_retired_transactions = 0;
	u64 m_gpu_vu_generated_private_retired_pairs = 0;
	u64 m_gpu_vu_generated_private_completion_count = 0;
	// Monotonic attestation of the only legal CPU evaluation point for a
	// generated successor: a real architectural observer/private-generation
	// commit. Hot accepted draws must leave this unchanged.
	u64 m_gpu_vu_generated_deferred_successor_evaluations = 0;
	std::array<u32, 4> m_gpu_vu_generated_private_completion_cycles{};
	// Worker-order micro writes provide a collision-free source generation for
	// this bounded product admission cache. An exact entry/configuration may
	// suppress speculative queue gathering, reuse a stable pre-effect rejection,
	// or remember that single-entry proof left room for continuation gathering.
	struct UniversalDispatchCostCacheEntry
	{
		bool valid = false;
		bool allow_multi_execute_gather = false;
		bool skip_single_attempt = false;
		bool wait_for_generated_program = false;
		bool wait_for_generated_compiler_idle = false;
		bool wait_for_generated_attestation = false;
		// Set only by an explicit terminal compiler/registration failure or a
		// rejected attestation. Cold output discovery and state-only Executes are
		// deliberately non-terminal and must not suppress another entry's GXP.
		bool generated_terminally_unavailable = false;
		// Once an exact source generation/entry/configuration has completed the
		// physical attestation gate and queued a product transaction, repeated
		// Executes can start from the immutable generated-bundle cache.  This is a
		// proof cache, not an admission whitelist: every live invocation still
		// resolves its control/output contract before effects, and any cache or
		// owner mismatch returns to full preflight.
			bool generated_product_ready = false;
			// A successfully proven NOP/IADDIU/LQI connector is compiled once into
			// an assignment formula. Repeated Executes use this cache directly and
			// do not rebuild an epoch or execute a CPU PairPlan/reference step.
			bool generated_state_formula_ready = false;
		u64 micro_generation = 0;
		u64 generated_program_identity = 0;
		u64 generated_key_low = 0;
		u64 generated_key_high = 0;
		u64 generated_attestation_key_low = 0;
		u64 generated_attestation_key_high = 0;
		u64 generated_compiler_idle_generation = 0;
		u32 entry_pc = 0;
		u32 configuration_bits = 0;
		u16 vif_top = 0;
		u16 vif_itop = 0;
		u32 generated_loop_kernel_abi = 0;
		u32 generated_semantic_profile_key = 0;
		u32 observer_fbrst = 0;
		u32 rejection = 0;
		u32 pair_count = 0;
			u32 dynamic_pair_upper_bound = 0;
			VitaGpuVu::UniversalStateLoadFormula generated_state_formula{};
	};
	// One already-attested generated Execute queued from the bounded MTVU
	// lookahead.  This is deliberately a compact accounting result: the draw
	// and its transaction move directly into their existing persistent owners
	// and no full VU state is returned to the caller.
	struct GeneratedProductHotQueueResult
	{
		u64 sequence = 0;
		u32 executed_pairs = 0;
		u32 vertices = 0;
		u32 primitives = 0;
		u32 resume_pc = 0;
	};
	// Pointer-free records decoded from the already-published MTVU ring while an
	// attested generated product owns the current microprogram generation. The
	// hot chain consumes these directly; it must not construct a universal epoch
	// request or revisit the cold provider hierarchy for every Execute.
	struct BufferedGeneratedExecute
	{
		s32 addr = 0;
		s32 end_pos = 0;
		u32 vif_top = 0;
		u32 vif_itop = 0;
		u32 fbrst = 0;
		u64 enqueued_at = 0;
		std::vector<VitaGpuVu::VifUnpackSpan> unpacks;
	};
	u64 m_gpu_vu_micro_generation = 1;
	// A VU source generation may have several runtime PATH1 contracts (GS debug
	// modes can change the live GIF tag without rewriting VU microcode). Once one
	// contract has queued an attested product, a terminal failure for a sibling
	// contract must not demote the whole source generation: returning to the
	// proven contract can reuse its cached GXP without compilation or analysis.
	bool m_gpu_vu_generated_generation_had_product = false;
	std::array<UniversalDispatchCostCacheEntry, 8>
		m_gpu_vu_dispatch_cost_cache{};
	u32 m_gpu_vu_dispatch_cost_cache_next = 0;
	std::vector<VitaGpuVu::VifUnpackSpan> m_deferred_vif_unpacks;
	std::atomic<u32> m_deferred_vif_unpack_count{0};
	u64 m_vif_span_sequence = 0;
	// EE-thread ownership. Complete captured UNPACK commands remain private until
	// the following VU execute or the end of the current VIF transfer publishes
	// the complete ordered group with one write-position update.
	bool m_pending_vif_batch = false;
	// EE-thread position of the last unpublished captured-span record. Exact
	// adjacent direct-affine V4-32 commands can extend it in place because the
	// worker cannot observe any record before CommitWritePos() publishes the batch.
	s32 m_pending_captured_vif_span_pos = -1;

	Threading::Thread m_thread;

public:
  struct ProducerProfileStats {
    u64 execute_enqueues;
    u64 wait_calls;
    u64 ring_waits;
    u64 ring_wait_spins;
    u64 compile_barriers;
    u64 queue_submissions;
    u64 queue_words;
  };

#if defined(VITASX2_QEMU_VALIDATION)
		// Host/physical-A9 validation of the compact private ownership models.
		// Product builds do not expose or execute this diagnostic entry point.
		static bool ValidateGeneratedLoopKernelDeferredRegisterOwners();
		static bool ValidateGeneratedLoopKernelLazyNativeMemoryOwners();
	#endif

	alignas(16)  vifStruct        vif;
	alignas(16)  VIFregisters     vifRegs;
	Threading::UserspaceSemaphore semaXGkick;
	std::atomic<unsigned int> vuCycles[4]; // Used for VU cycle stealing hack
	u32 vuCycleIdx;  // Used for VU cycle stealing hack
	u32 vuFBRST;

	enum InterruptFlag {
		InterruptFlagFinish = 1 << 0,
		InterruptFlagSignal = 1 << 1,
		InterruptFlagLabel  = 1 << 2,
		InterruptFlagVUEBit = 1 << 3,
		InterruptFlagVUTBit = 1 << 4,
	};

	std::atomic<u32> mtvuInterrupts; // Used for GS Signal, Finish etc, plus VU End/T-Bit
	std::atomic<u64> gsLabel; // Used for GS Label command
	std::atomic<u64> gsSignal; // Used for GS Signal command

	VU_Thread();
	~VU_Thread();

	__fi const Threading::ThreadHandle& GetThreadHandle() const { return m_thread; }

	/// Returns true if the VU thread has been started.
	__fi bool IsOpen() const { return m_thread.Joinable(); }

	/// Ensures the VU thread is started.
	void Open();

	/// Shuts down the VU thread if it is currently running.
	void Close();

	void Reset();

	// Rebuilds the drained worker's host-private mirror after portable replay
	// restores the canonical VIF/VU1 state. This is exactly the publication
	// sequence used by native SaveStateBase::mtvuFreeze() on load.
	void RebuildFromCanonicalStateAfterPortableLoad();

	// Get MTVU to start processing its packets if it isn't already
	void KickStart();

	// Used for assertions...
	bool IsDone();

	// Waits till MTVU is done processing
	void WaitVU();

	void Get_MTVUChanges();
	__fi bool HasPendingChanges() const
	{
		return mtvuInterrupts.load(std::memory_order_acquire) != 0;
	}
	__fi ProducerProfileStats GetProducerProfileStats() const
	{
		return {m_profile_execute_enqueues, m_profile_wait_calls,
			m_profile_ring_waits, m_profile_ring_wait_spins,
			m_profile_compile_barriers, m_profile_queue_submissions,
			m_profile_queue_words};
	}
	struct ProducerQueueHealth
	{
		u64 enqueued, completed;
		u32 read, published_write, private_write, pending_vif;
	};
	// Only CPU0 may read private_write/pending_vif. Other fields are atomics.
	ProducerQueueHealth GetProducerQueueHealth() const
	{
		return {m_execute_jobs_enqueued.load(std::memory_order_relaxed),
			m_execute_jobs_completed.load(std::memory_order_relaxed),
			static_cast<u32>(m_ato_read_pos.load(std::memory_order_relaxed)),
			static_cast<u32>(m_ato_write_pos.load(std::memory_order_relaxed)),
			static_cast<u32>(m_write_pos), m_pending_vif_batch ? 1u : 0u};
	}

	// These methods are VU-worker-only. Release publication in EndProgram()
	// makes completed VU state visible before the EE observes its event bit.
	__fi bool IsProgramActive() const { return m_program_active; }
	void BeginProgram();
	void MarkDtProgramEnd(u32 interrupt_flag);
	void EndProgram(u32 interrupt_flag);

	void ExecuteVU(u32 vu_addr, u32 vif_top, u32 vif_itop, u32 fbrst);

	void VifUnpack(vifStruct& _vif, VIFregisters& _vifRegs, const u8* data, u32 size);

	// Publishes captured UNPACK commands which have not yet been joined to a VU
	// execute. VIF1 calls this at a transfer boundary; input-ring pressure and
	// architectural waits use the same seam before waiting on another owner.
	void PublishPendingVifBatch();

	// Requests non-blocking publication of a worker-private direct PATH1 prefix.
	// The final generated descriptor remains private until a later visibility
	// boundary supplies its ordered submission marker.
	void RequestGpuVuPath1Publication();
	// Requests publication plus an early vertex-visibility boundary. VSync uses
	// this because its command can wait behind the same counted PATH1 reservation;
	// architectural observers and genuine resource pressure use it as well.
	void RequestGpuVuPath1Flush();
	// GXM completion and ordered PATH1 retirement wake CPU1 through this seam.
	// It is a notification only and never executes VU work on the caller.
	void NotifyUniversalGpuVuProgress();
	// Called only after the producer has published a complete VSync command.
	// Bounds guest-visible latency without stopping inside a VIF command chain
	// that the GS worker may need in order to retire universal work.
	void BoundGpuVuExecutionLatencyAtFrameBoundary();

	// A generated transaction whose exact successor has been accepted can own
	// the next VIF1 MSCAL/MSCNT dependency without first materializing VU1 on
	// ARM. This does not make its state guest-visible: EE/VU-memory observers,
	// reset, savestate, and shutdown still use WaitVU().
	__fi bool CanChainVifBehindGeneratedGpuVu() const
	{
		return m_gpu_vu_generated_pending_transaction_count.load(
			std::memory_order_acquire) != 0u;
	}

	// Writes to VU's Micro Memory (size in bytes)
	void WriteMicroMem(u32 vu_micro_addr, const void* data, u32 size);

	// Writes to VU's Data Memory (size in bytes)
	void WriteDataMem(u32 vu_data_addr, const void* data, u32 size);

	void WriteVIRegs(REG_VI* viRegs);

	void WriteVFRegs(VECTOR* vfRegs);

	void WriteCol(vifStruct& _vif);

	void WriteRow(vifStruct& _vif);

private:
	void ExecuteRingBuffer();
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	void PublishGpuVuHealth(u32 stage, u32 wait_poll_count = 0u);
#endif

	void WaitOnSize(s32 size);
	void ReserveSpace(s32 size);

	s32 GetReadPos();
	s32 GetWritePos();

	u32* GetWritePtr();

	void CommitWritePos();
	void CommitReadPos();

	u32 Read();
	void Read(void* dest, u32 size);
	void ReadRegs(VIFregisters* dest);

	void Write(u32 val);
	void Write(const void* src, u32 size);
	void WriteRegs(VIFregisters* src);

	u32 Get_vuCycles();
	void PrepareVuCodeForExecute(s32 vu_addr);
	void WaitForQueue();
	void AppendDeferredVifUnpack(VitaGpuVu::VifUnpackSpan span);
	void ReplayDeferredVifUnpacks();
	void ReplayVifUnpackSpans(
		std::vector<VitaGpuVu::VifUnpackSpan>* spans);
	void ReleaseDeferredVifUnpacks();
	bool DrainUniversalGpuVuEpochs(bool wait_for_all);
	bool DrainGeneratedLoopKernelTransactions(
		bool wait_for_all, bool commit_private_state);
	bool AdoptGeneratedLoopKernelTransactionBatch();
	bool RetireCompletedGeneratedLoopKernelTransactions();
	bool MaterializeGeneratedLoopKernelDeferredState();
	bool MaterializeGeneratedLoopKernelDeferredMemory();
	bool CommitGeneratedLoopKernelPrivateState();
	void RecordGeneratedLoopKernelPrivateCompletion(u32 cycle);
	bool CanAdvanceGeneratedLoopKernelPrivateState(
		const VitaGpuVu::GeneratedLoopKernelTransaction& transaction,
		const std::vector<VitaGpuVu::VifUnpackSpan>& unpacks,
		GeneratedLoopKernelPrivateAdvanceProof* proof) const;
	void AdvanceGeneratedLoopKernelPrivateState(
		const VitaGpuVu::GeneratedLoopKernelTransaction& transaction,
		const std::vector<VitaGpuVu::VifUnpackSpan>& unpacks,
		const GeneratedLoopKernelPrivateAdvanceProof& proof,
		const VitaGpuVu::GeneratedLoopKernelFinalStateValues*
			resolved_successor = nullptr);
	bool TryQueueGeneratedLoopKernelProductHotExecute(
		UniversalDispatchCostCacheEntry& cache, u32 entry_pc,
		u16 vif_top, u16 vif_itop,
		std::vector<VitaGpuVu::VifUnpackSpan>* unpacks,
		GeneratedProductHotQueueResult* result,
		std::string* error = nullptr);
	bool TryQueueAttestedGeneratedProductHotChain(
		UniversalDispatchCostCacheEntry& base_cache, u32 base_entry_pc,
		bool base_resume, u16 base_vif_top, u16 base_vif_itop,
		std::vector<VitaGpuVu::VifUnpackSpan>* base_unpacks,
		std::vector<BufferedGeneratedExecute>* buffered_executes);
	bool TryQueueGeneratedPrivateStateLoadContinuation(
		const VitaGpuVu::UniversalGpuVuEpoch& epoch, u32 fbrst,
		bool resume, bool allow_general_no_output = false,
		VitaGpuVu::UniversalStateLoadFormula* compiled_formula = nullptr);
	bool TryQueueGeneratedStateLoadFormulaContinuation(
		const VitaGpuVu::UniversalStateLoadFormula& formula, bool resume,
		u64* queued_sequence_out = nullptr);
	bool ApplyGeneratedPrivateStateLoadContinuations(u64 predecessor_sequence);
	u32 GeneratedLoopKernelPhysicalTransactionCount() const;
	u32 GeneratedLoopKernelPendingExecutionCount() const;
	void PublishGeneratedLoopKernelPendingExecutionCount();
	void InvalidateGeneratedLoopKernelPrivateState();
	void ReleaseRetiredUniversalGpuVuEpochs();
	bool MaterializeUniversalGpuVuStateForCpu();

	std::atomic<u32> m_gpu_vu_path1_publication_requests{0u};
};

extern VU_Thread vu1Thread;
