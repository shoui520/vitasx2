// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "pcsx2/HostMemoryMap.h"
#include "pcsx2/MemoryTypes.h"
#include "pcsx2/vita/A32Emitter.h"
#include "pcsx2/vita/VitaEeBlockCompiler.h"
#if defined(VITASX2_QEMU_VALIDATION)
#include "pcsx2/vita/VitaCore.h"
#endif

#include <array>
#include <cstddef>
#include <memory>
#include <vector>
#if defined(VITASX2_QEMU_VALIDATION)
#include <unordered_map>
#endif

namespace VitaEE
{
	enum class BlockExecutionPath : u8
	{
		Compiled,
		InterpreterStep,
	};

	enum class BlockExitKind : u32
	{
		Direct = 0xd1,
		Event = 0xe7,
		InterpreterStep = 0x1e,
	};

	enum class BlockScanStop : u8
	{
		UnsupportedOpcode,
		PageBoundary,
		DebugBoundary,
		BranchTargetBoundary,
		ExistingBlockBoundary,
		OpcodeBoundary,
		Branch,
		MaxInstructions,
		AddressWrap,
	};

	struct BlockScanResult
	{
		u32 start_pc = 0;
		u32 instruction_count = 0;
		u32 stop_pc = 0;
		BlockScanStop stop = BlockScanStop::UnsupportedOpcode;
	};

	// Immutable description of one already-generated PCSX2/Vita tier-zero
	// source fragment. Region formation consumes this instead of reconstructing
	// scheduler seams from raw opcodes: host code-budget splits and PCSX2 short
	// concatenations are execution contracts, not ISA properties.
	struct RegionSourceBlockContract
	{
		u32 start_pc = 0;
		u32 instruction_count = 0;
		u32 dependency_start_pc = 0;
		u32 dependency_instruction_count = 0;
		u32 charged_scaled_cycles_before = 0;
		bool scheduler_test_at_end = true;
		bool specialized_wait = false;
	};

	// Immutable direct CFG topology owned by the same generated tier-zero block.
	// PCSX2's BaseBlocks link records are the authority: region discovery may use
	// these targets to assemble a natural loop whose blocks are not contiguous in
	// guest address space, but it may never invent an edge from raw address order.
	struct RegionSourceBlockTopology
	{
		RegionSourceBlockContract contract{};
		std::array<u32, 2> successors{};
		u8 successor_count = 0;
	};

	struct RegionDirectCallPredecessors
	{
		static constexpr u8 CAPACITY = 8;
		std::array<u32, CAPACITY> entry_pcs{};
		u8 count = 0;
	};

	// Already-published PCSX2/Vita tier-zero owners with a direct edge into one
	// source block.  This is deliberately broader than the JAL-only query above:
	// cold region discovery uses it to walk from a sampled interior block back to
	// the source-backed function/caller entry without decoding speculative RAM or
	// installing a counter on every generated edge.
	struct RegionSourceBlockPredecessors
	{
		static constexpr u8 CAPACITY = 8;
		std::array<u32, CAPACITY> entry_pcs{};
		u8 count = 0;
		bool truncated = false;
	};

	struct BlockExecutionResult
	{
		BlockExecutionPath path = BlockExecutionPath::Compiled;
		BlockExitKind exit = BlockExitKind::Direct;
		u32 exit_value = 0;
		// Guest entry selected for this provider dispatch. This is authoritative
		// at an unlinked/cold boundary; a direct-linked chain may execute later
		// blocks before returning, so consumers must still prove the observed edge
		// from its terminating opcode before treating it as a region candidate.
		u32 start_pc = 0;
		u32 instruction_count = 0;
		// The scanner may expose a larger straight-line region than fits the
		// bounded A32 code slice. Retain the original PCSX2 logical region size so
		// validation can prove that an A32 physical fragment did not become an
		// extra scheduler boundary or an interpreter fallback.
		u32 source_instruction_count = 0;
		u32 scaled_cycles = 0;
		size_t code_size = 0;
		u32 block_records = 0;
		u32 link_records = 0;
		u32 cache_slots = 0;
		u32 code_cache_resets = 0;
		size_t code_cache_used = 0;
		size_t code_cache_capacity = 0;
		bool cache_hit = false;
		bool lookup_hit = false;
		bool fast_dispatch_hit = false;
		// Set from the token emitted by the generated block which actually
		// returned to the dispatcher. Unlike the prepared-entry telemetry below,
		// this remains authoritative after one or more patched direct links.
		bool scheduler_test_elided = false;
		// A region exact suffix may complete an operation which invalidated its
		// own executable owner. Its next dispatcher seam must unwind before any
		// generated lookup or ownership publication can continue.
		bool persistent_resume_requires_boundary = false;
#if defined(VITASX2_QEMU_VALIDATION)
		u32 concatenated_short_blocks = 0;
		u32 concatenated_short_scheduler_tests_elided = 0;
		u32 concatenated_short_hot_instructions_elided = 0;
		u32 code_budget_continuation_blocks = 0;
		u32 code_budget_continuation_scheduler_tests_elided = 0;
		u32 code_budget_continuation_hot_instructions_elided = 0;
		u32 generated_frame_pushes = 0;
		u32 generated_frame_pops = 0;
		u32 dispatcher_frame_pushes = 0;
		u32 dispatcher_frame_pops = 0;
		u32 resident_self_links = 0;
		u32 resident_self_link_entry_instructions = 0;
		u32 resident_self_link_entry_loads = 0;
		u32 compatible_gpr_links = 0;
		u32 compatible_generated_region_links = 0;
		u32 post_writeback_canonical_links = 0;
		u32 compatible_gpr_link_entry_instructions = 0;
		u32 compatible_gpr_link_entry_loads = 0;
		u32 compatible_gpr_words_carried = 0;
		u32 compatible_gpr_dirty_words_carried = 0;
		u32 compatible_gpr_chain_blocks = 0;
		u32 compatible_scheduler_links = 0;
		u32 compatible_vtlb_pointer_links = 0;
		u32 embedded_compatible_continuations = 0;
#endif
	};

	struct PersistentGeneratedEntry
	{
		// Admission probes are dispatcher-visible owners, but unlike compiled
		// regions they must not replace edges originating inside the candidate
		// CFG.  Otherwise a loop's tier-zero backedge re-enters the proof on every
		// iteration and turns cold admission into permanent hot-path work.
		// Phase 4 caller/leaf regions commonly cross several direct-call return
		// blocks before reaching the enclosing latch.  Twenty-four can represent a
		// 16-source-block loop plus the separately attested direct leaves permitted
		// by LiftOptions.  This is an ownership/IR ceiling, not a code-size policy:
		// the backend still independently enforces its 4 KiB hot slab, 1.5 KiB entry,
		// 3 KiB body, allocation-pressure and physical-A9 profitability gates.
		static constexpr u8 MAX_INTERNAL_SOURCE_BLOCKS = 24;

		u32 pc = 0;
		// Indirect dispatch and signature-mismatched direct edges always enter the
		// canonical form.  A direct edge may select compatible_entry_point only
		// after exact equality with compatible_signature proves every private host
		// value and representation.
		const void* entry_point = nullptr;
		const void* compatible_entry_point = nullptr;
		GprLinkSignature compatible_signature{};
		std::array<u32, MAX_INTERNAL_SOURCE_BLOCKS> internal_source_blocks{};
		u8 internal_source_block_count = 0;
		bool external_incoming_only = false;

		bool BypassesSource(u32 source_pc) const
		{
			if (!external_incoming_only)
				return false;
			for (u8 index = 0; index < internal_source_block_count; index++)
			{
				if (internal_source_blocks[index] == source_pc)
					return true;
			}
			return false;
		}
	};

	struct PersistentRegionAbi
	{
		const void* scheduler_elided_redispatch = nullptr;
		const void* event_exit = nullptr;
		const void* generated_failure_exit = nullptr;
		u32* resumed_fragment_active = nullptr;
	};

	// A normalized, target-independent proof that the current architectural loop
	// state has enough remaining iterations to amortize a generated region. The
	// RegionMemoryPlan analyzer owns its construction; BlockExecutor only emits the
	// corresponding A32 comparison before incrementing an admission counter.
	struct PersistentProbeIterationGuard
	{
		enum class Kind : u8
		{
			DecrementCounter,
			IncreasingEndpoint,
		};

		Kind kind = Kind::DecrementCounter;
		u8 counter_gpr = 0;
		u8 bound_gpr = 0;
		bool bound_is_immediate = false;
		bool require_counter_nonnegative = false;
		bool require_bound_nonnegative = false;
		u32 bound_immediate = 0;
		// Counter minimum for DecrementCounter; remaining-distance minimum for
		// IncreasingEndpoint. Zero is valid only for the latter.
		u32 threshold = 0;
		// Power-of-two recurrence divisibility requirement, encoded as step - 1.
		u32 alignment_mask = 0;
	};

	class BlockExecutor
	{
	public:
		using PersistentBoundaryCallback = bool (*)(void* userdata,
			const BlockExecutionResult& completed_chain);
		// Called only after a previously absent PCSX2-discovered block has been
		// compiled, and before generated execution enters it. Returning false
		// unwinds the private dispatcher without executing the prepared block.
		// Region discovery uses this cold seam because an immediately patched
		// backedge otherwise never reaches PersistentBoundaryCallback.
		using PersistentPreparedCallback = bool (*)(void* userdata,
			const BlockExecutionResult& prepared_block);
		// Runs the complete PCSX2 EE event owner and returns nonzero only when the
		// active generated lookup may resume inside the persistent JIT frame.
		using PersistentEventCallback = u32 (*)(u32 event_token);

		// PCSX2 recRecompile() discovers through the next branch, existing
		// BaseBlock, debugger seam, or 4 KiB source-page boundary. A branch in the
		// final page word still owns its delay slot on the following page, hence
		// 1024 page words plus one atomic follower. Host-code budget splitting is a
		// separate concern below; imposing a smaller discovery ceiling changes
		// fixed-point cycle rounding and therefore Count/event timing.
		static constexpr u32 MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS = 1025;
		static_assert(MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS ==
			BlockCompiler::MAX_COMPILE_INSTRUCTIONS);
		// Exact owners and reference counts remain grouped by PCSX2's 4 KiB
		// source pages. The generated hot path uses a byte per arena page. Its
		// page-positive cold path uses one bit per 64-byte retail-RAM chunk so
		// data writes sharing a page with code avoid the exact-overlap callback.
		static constexpr u32 RAM_SOURCE_PAGE_SHIFT = 12;
		static constexpr u32 RAM_SOURCE_PAGE_COUNT =
			Ps2MemSize::MainRam >> RAM_SOURCE_PAGE_SHIFT;
		static constexpr u32 RAM_SOURCE_CHUNK_SHIFT =
			BlockCompiler::RAM_SOURCE_GUARD_CHUNK_SHIFT;
		static constexpr u32 RAM_SOURCE_CHUNK_COUNT =
			Ps2MemSize::MainRam >> RAM_SOURCE_CHUNK_SHIFT;
		static constexpr u32 RAM_WRITE_GUARD_PAGE_COUNT =
			HostMemoryMap::MainSize >> RAM_SOURCE_PAGE_SHIFT;
		static constexpr u32 RAM_SOURCE_CHUNK_LIVE_BIT_BYTES =
			(RAM_SOURCE_CHUNK_COUNT + 7) / 8;
		static_assert((HostMemoryMap::MainSize &
			((1u << RAM_SOURCE_PAGE_SHIFT) - 1)) == 0);
		static_assert(RAM_SOURCE_PAGE_COUNT <= RAM_WRITE_GUARD_PAGE_COUNT);

		BlockExecutor();
		~BlockExecutor();

		u32 Shutdown();
		u32 Reset();
		u32 InvalidateRange(u32 start_pc, u32 instruction_count);
		// backing_start is a byte offset into eeMem->Main, not a guest virtual
		// address.  This is deliberately separate from InvalidateRange(): several
		// physical/KSEG/TLB aliases can own the same compiled source bytes.
		u32 InvalidateRamSourceRange(u32 backing_start, u32 size);
		const u8* RamSourcePageLiveFlags() const
		{
			return m_ram_source_page_live_flags.data();
		}
		const u8* RamSourceChunkLiveBits() const
		{
			return m_ram_source_chunk_live_bits.data();
		}
		bool IsRamSourceChunkLive(u32 backing_offset) const;
		void SetDirectLinkingEnabled(bool enabled);
		// A helper can request a whole-cache reset while generated code is still
		// executing. Stop dynamic lookup immediately without patching the current
		// code page; Reset() and the next directory allocation republish it.
		void SuspendGeneratedLookupUntilReset();
		void SetPersistentDispatchEnabled(bool enabled);
		bool PersistentDispatchEnabled() const
		{
			return m_persistent_dispatch_enabled;
		}
		// A persistent block normally enters its next block without returning to
		// the provider. PCSX2's x86 recRecompile() embeds a few lifecycle hooks at
		// specific block entries; A32 keeps those entries on the private
		// dispatcher boundary so the provider can run the same hooks before the
		// target block executes, without disabling linking for the rest of boot.
		static u32 CanonicalizeRamBackedPc(u32 pc);
		bool SetPersistentDispatchBarrier(u32 pc, bool enabled);
		// Read-only with respect to generated code: succeeds only for a live,
		// PCSX2-discovered tier-zero block already present in this executor.
		// The caller must retry at a later natural boundary when a CFG member has
		// not executed yet; this method never compiles speculative blocks.
		bool GetRegionSourceBlockContract(
			u32 start_pc, RegionSourceBlockContract* contract);
		bool GetRegionSourceBlockTopology(
			u32 start_pc, RegionSourceBlockTopology* topology);
		// Return already-published PCSX2 source owners whose terminal direct JAL
		// targets this entry. This is a cold discovery query over BaseBlocks' sorted
		// incoming-link records; it never scans guest RAM or compiles a predecessor.
		bool GetRegionDirectCallPredecessors(u32 callee_pc,
			RegionDirectCallPredecessors* predecessors);
		bool GetRegionSourceBlockPredecessors(u32 target_pc,
			RegionSourceBlockPredecessors* predecessors);
		// Reserve generated region code from the EE cache which owns the source
		// contracts and source generation. Vita's executable arena is allocated in
		// 1 MiB logical slices; allocating a standalone 4 KiB CodeBuffer would
		// otherwise consume a whole slot after EE/IOP/VU have claimed the fixed
		// 22 MiB product layout. Region/probe code lives in fixed slots at the top
		// of the EE arena so LRU metadata replacement cannot consume the tier-zero
		// bump cache indefinitely. A committed slot is retired while its old lookup
		// owner may still be reachable, and becomes reusable only after the outer
		// provider has replaced the complete generated directory and repatched every
		// incoming/generated edge.
		static constexpr size_t REGION_CODE_BUFFER_CAPACITY = 32 * 1024;
		static constexpr size_t REGION_PROBE_CODE_BUFFER_CAPACITY = 512;
		static constexpr size_t PERSISTENT_REGION_CONTINUATION_SLOT_COUNT = 32;
		static constexpr size_t PERSISTENT_REGION_CONTINUATION_CODE_CAPACITY =
			32 * 1024;
		bool PrepareRegionCodeBuffer(VitaA32::CodeBuffer* code, size_t capacity,
			size_t* slice_offset);
		bool CommitRegionCodeBuffer(size_t slice_offset, size_t code_size);
		void DiscardRegionCodeBuffer(VitaA32::CodeBuffer* code,
			size_t slice_offset);
		bool RetireRegionCodeBuffer(VitaA32::CodeBuffer* code,
			size_t slice_offset);
		void AcknowledgeRegionCodeRetirements();
		// RegionRuntime calls this at the outer boundary after it has retired every
		// generated owner for a changed source/config generation. Bytes remain
		// immutable until the rebuilt generated directory no longer references them;
		// AcknowledgeRegionCodeRetirements() then makes the slots reusable.
		void RetirePersistentRegionContinuations();
#if defined(VITASX2_QEMU_VALIDATION)
		size_t GetCommittedRegionCodeSlotCountForValidation() const;
		size_t GetRetiredRegionCodeSlotCountForValidation() const;
#endif
		// Append one callable suffix of an already-attested PCSX2 source block to
		// the region's private code allocation. inherited_raw_cycles is the exact
		// unscaled cost of the Region IR prefix which has already committed its
		// architectural effects. The suffix owns the sole scale/publication and
		// the original source fragment's scheduler policy.
		bool AppendRegionContinuation(VitaA32::CodeBuffer* code, u32 start_pc,
			u32 instruction_count, u32 inherited_raw_cycles,
			bool scheduler_test_at_end, bool persistent_dispatch_fragment,
			size_t* entry_offset,
			u32* scaled_cycles = nullptr);
		// Resolve one exact mid-block/nonzero-debt continuation as an immutable
		// first-class persistent-dispatch fragment. The fragment is shared across
		// regions with the same PCSX2 source/timing contract instead of being copied
		// into every region's 32 KiB private slot. Compilation and publication are
		// cold outer-boundary operations; returned code remains immutable until the
		// complete EE code cache is reset.
		bool GetOrCreatePersistentRegionContinuation(u32 start_pc,
			u32 instruction_count, u32 inherited_raw_cycles,
			bool scheduler_test_at_end, const void** entry_point,
			u32* scaled_cycles = nullptr);
		// Execute a previously appended callable suffix. Event work is performed
		// exactly once here when requested; scheduler_test_elided reports an
		// attested physical/PCSX2 continuation seam rather than a guest event seam.
		bool ExecuteRegionContinuation(const VitaA32::CodeBuffer& code,
			size_t entry_offset, bool run_event_test,
			BlockExitKind* exit, bool* scheduler_test_elided = nullptr);
		// Reconcile the complete set of lifecycle owners as one unique union.
		// Required barriers are installed before stale barriers are removed, so a
		// failed code patch leaves dispatch conservatively on provider boundaries.
		bool SetPersistentDispatchBarriers(const u32* pcs, size_t count);
		void ClearPersistentDispatchBarriers();
		// Active regions are first-class generated-dispatch entries, not provider
		// barriers. Publish the complete desired set only at an outer boundary.
		// Incoming links retain their existing canonicalization leaves and are
		// repatched to these entries only after their private state is materialized.
		bool SetPersistentGeneratedEntries(
			const PersistentGeneratedEntry* entries, size_t count);
		void ClearPersistentGeneratedEntries();
#if defined(VITASX2_QEMU_VALIDATION)
		u64 GetPersistentGeneratedPublicationPatchCountForValidation() const
		{
			return m_persistent_generated_publication_patches;
		}
#endif
		bool GetPersistentRegionAbi(PersistentRegionAbi* abi);
		// Resolve the current first-class dispatcher owner for a static guest PC.
		// Region compilation calls this only outside a live dispatcher frame;
		// source-generation synchronization retires the region before a returned
		// tier-zero entry can be invalidated or reused.
		const void* GetPersistentDispatchEntryPoint(u32 start_pc);
		// Resolve a target which accepts the exact private scalar contract already
		// materialized by a generated predecessor. No canonical fallback is
		// returned: callers must choose their separately emitted canonical exit.
		const void* GetPersistentCompatibleDispatchEntryPoint(u32 start_pc,
			const GprLinkSignature& signature);
		// Resolve the immutable tier-zero compatible entry without consulting a
		// first-class region override. A region entry guard which has executed no
		// guest instruction uses this to reject an unprofitable invocation while
		// preserving the predecessor's exact private register contract.
		const void* GetPersistentCompatibleTierZeroEntryPoint(u32 start_pc,
			const GprLinkSignature& signature);
		// Returns the exact already-compiled tier-zero entry. This never compiles,
		// mutates ownership, or consults a generated region override.
		const void* GetPersistentTierZeroEntryPoint(u32 start_pc);
		// Emit a canonical generated-directory probe which samples a bounded number
		// of entries and then transparently enters the immutable tier-zero owner.
		// BlockExecutor owns the private dispatcher register/PC contract;
		// RegionRuntime supplies only the
		// bounded admission state. This must be called outside a live dispatcher
		// frame and published only after the returned code buffer has been synced.
		bool AppendPersistentTierZeroProbe(VitaA32::CodeBuffer* code,
			u32 start_pc, u32* observations, u32* observation_epoch,
			const u32* current_epoch, u32* samples,
			volatile u32* promotion_requested,
			u32 promotion_threshold, u32 sample_budget,
			const PersistentProbeIterationGuard* iteration_guard,
			size_t* entry_offset,
			size_t* compatible_entry_offset,
			GprLinkSignature* compatible_signature);
		// Returns the private register contract owned by the already-compiled
		// tier-zero target. Region publication may consume this only at an outer
		// ownership boundary; an exact signature match is required before any
		// incoming link may bypass canonical state.
		bool GetPersistentTierZeroLinkSignature(u32 start_pc,
			GprLinkSignature* signature);
#if defined(VITASX2_QEMU_VALIDATION)
		const void* GetGeneratedLookupEntryForValidation(u32 pc) const;
		// Replace only the generated-directory root selected by the validation
		// dispatcher. Direct-link patches and ownership metadata remain untouched,
		// allowing an adversary to enter an immutable tier-zero predecessor once
		// and prove its already-patched private-state edge into a generated region.
		bool SetGeneratedLookupEntryForValidation(u32 pc, const void* entry_point);
		bool GetCompiledBlockEvidenceForValidation(u32 pc,
			BlockExecutionResult* result);
		void SetCompatibleGprDirtyCarryEnabled(bool enabled);
		void SetCompatibleSchedulerCarryEnabled(bool enabled);
		void SetCompatibleVtlbPointerCarryEnabled(bool enabled);
		void SetCompatibleVtlbHostReclaimEnabled(bool enabled);
		void SetCompatiblePredicateCarryEnabled(bool enabled);
		void SetCompatibleLikelyTakenSuffixEnabled(bool enabled);
		void SetCompatiblePredicateEntryVariantEnabled(bool enabled);
		void SetEmbeddedCompatibleContinuationEnabled(bool enabled);
		void SetFusedDirectEventLinkEnabled(bool enabled);
		void SetCombinedCompatibleTakenEventEnabled(bool enabled);
		void SetCompatibleVtlbWriteGuardHoistEnabled(bool enabled);
		void SetCompatibleVtlbReadGuardHoistEnabled(bool enabled);
		void SetSingleBlockGprLinkEnabled(bool enabled);
		void SetReciprocalJumpGprLinkEnabled(bool enabled);
		void SetThreeBlockGprLinkEnabled(bool enabled);
		void SetVtlbLinkedEntryPcPublicationEnabled(bool enabled);
		void SetDirectLinkRejectionProfileEnabled(bool enabled);
		void ResetDirectLinkRejectionProfile();
		VitaA32EeLinkRejectionProfile GetDirectLinkRejectionProfile() const;
#endif
		static bool ScanStraightLineBlock(u32 start_pc, u32 max_instruction_count, BlockScanResult* result);
		bool ExecuteCompiledBlock(u32 start_pc, u32 instruction_count,
			bool run_event_test_on_event_exit, BlockExecutionResult* result,
			bool allow_code_budget_split = false);
		bool ExecuteCompiledBlockAtPc(u32 start_pc, bool run_event_test_on_event_exit,
			BlockExecutionResult* result);
		bool ExecutePersistentAtPc(u32 start_pc, bool run_event_test_on_event_exit,
			PersistentBoundaryCallback boundary_callback, void* callback_userdata,
			BlockExecutionResult* result,
			PersistentEventCallback event_callback = nullptr,
			PersistentPreparedCallback prepared_callback = nullptr);
		// A dispatcher-ABI region may already have committed memory effects when
		// an impossible token or exact-suffix failure is detected. The provider
		// must unwind fatally rather than retrying that guest PC in tier zero.
		bool ConsumePersistentGeneratedFailure();
		bool ExecuteStraightLineBlockOrInterpreterStep(u32 start_pc, u32 instruction_count,
			bool run_event_test_on_event_exit, BlockExecutionResult* result);
		u32 GetCodeCacheResetCount() const { return m_code_cache_resets; }
		u32 GetSourceGeneration() const { return m_source_generation; }
		size_t GetCodeCacheUsed() const { return m_code_cache_used; }
		size_t GetCodeCacheCapacity() const { return m_code_cache_capacity; }
		u32 GetCodeCacheBlockRecordCount() const
		{
			return static_cast<u32>(m_block_records.size());
		}
		u32 GetCodeCacheSlotCount() const
		{
			return static_cast<u32>(m_cache.size());
		}
		u32 GetCodeCacheSlotMetadataSize() const
		{
			return static_cast<u32>(sizeof(CachedBlock));
		}

	private:
		// PCSX2 owner: x86/BaseblockEx.h::BaseBlocks() starts at 0x4000
		// BASEBLOCKEX records and grows from there. Vita keeps the same
		// game-scale order while bounding metadata for the smaller memory budget.
		static constexpr size_t INITIAL_CACHE_CAPACITY = 512;
		static constexpr size_t MAX_CACHE_CAPACITY = 0x8000;
		static constexpr size_t STRAIGHT_LINE_BLOCK_CODE_CAPACITY = 4096;
		// PCSX2 x86/ix86-32/iR5900.cpp::recRecompile() uses its split-block
		// continuation when a block must remain manageable.  A32 first grows the
		// temporary slice for ordinary variance, then turns an over-budget region
		// into two normal directly-linkable blocks instead of admitting a single
		// pathological body into the Cortex-A9 instruction cache.
		static constexpr size_t MAX_STRAIGHT_LINE_BLOCK_CODE_CAPACITY = 32 * 1024;
		static constexpr size_t EE_CODE_CACHE_CAPACITY = 14 * 1024 * 1024;
		static constexpr size_t EE_FALLBACK_CODE_CACHE_CAPACITY =
			HostMemoryMap::EErecSize;
		static constexpr size_t CODE_CACHE_ALIGNMENT = 32;
		// One staging region lets a replacement compile while all 32 published
		// owners remain reachable. Probe maintenance can retire and replace the
		// complete 32-owner directory in one outer transaction, hence two banks.
		static constexpr size_t REGION_CODE_SLOT_COUNT = 33;
		static constexpr size_t REGION_PROBE_CODE_SLOT_COUNT = 64;
		static constexpr size_t REGION_CODE_ARENA_CAPACITY =
			REGION_CODE_SLOT_COUNT * REGION_CODE_BUFFER_CAPACITY +
			REGION_PROBE_CODE_SLOT_COUNT * REGION_PROBE_CODE_BUFFER_CAPACITY;
		static constexpr size_t DIRECT_LINK_SLOT_COUNT = 2;
		static constexpr size_t MAX_INCOMING_LINKS = MAX_CACHE_CAPACITY * DIRECT_LINK_SLOT_COUNT;
		static constexpr u32 LOOKUP_DIRECTORY_ENTRY_COUNT = 0x10000;
		static constexpr u32 LOOKUP_PAGE_ENTRY_COUNT = 0x4000;
		static constexpr u32 INVALID_RAM_SOURCE = UINT32_MAX;
		// A maximum-size aligned EE block covers at most the tail of one vTLB
		// page and the complete following page. Keep one spare fragment so this
		// invariant remains robust if the scanner's atomic follower grows.
		// A page-straddling ordinary block needs at most three fragments. A
		// poll-call wait additionally owns its call pair and disjoint leaf; each
		// two-word range can itself straddle a page.
		static constexpr u32 MAX_RAM_SOURCE_FRAGMENTS = 7;

		struct RamSourceFragment
		{
			u32 start = INVALID_RAM_SOURCE;
			u32 size = 0;
		};

		struct CachedBlock
		{
			CachedBlock* next_free = nullptr;
			VitaA32::CodeBuffer code;
			// Match recRAMCopy without charging every cached block for a worst-case
			// page-sized inline array. The nothrow allocation is released with the
			// BaseBlock, so cold long blocks cannot leave page-sized capacity behind
			// in every recycled Vita cache slot.
			std::unique_ptr<u32[]> opcodes;
			u32 start_pc = 0;
			u32 instruction_count = 0;
			u32 source_instruction_count = 0;
			u32 dependency_start_pc = 0;
			u32 dependency_instruction_count = 0;
			u32 dependency_charged_cycles_before = 0;
			// The exact wait-loop proofs are rare, cold metadata. Keeping both
			// worst-case records inline charged every ordinary EE block for data it
			// could never consume; PES retained nearly 30,000 cache slots across a
			// code-cache rewind. Allocate only the proof actually emitted so the
			// process-lifetime slot pool does not exhaust newlib before the next
			// generation can reuse it.
			std::unique_ptr<PollCallWaitLoopSourceProof>
				poll_call_wait_loop_source_proof;
			std::unique_ptr<TwoPredicateWaitLoopSourceProof>
				two_predicate_wait_loop_source_proof;
			std::array<RamSourceFragment, MAX_RAM_SOURCE_FRAGMENTS>
				ram_source_fragments{};
			u32 source_serial = 0;
			u8 ram_source_fragment_count = 0;
			u32 scaled_cycles = 0;
			s8 ee_cycle_rate = 0;
			u8 cp0_config_cycle_shift = 0;
			size_t linked_entry_offset = 0;
			size_t resident_self_link_entry_offset = static_cast<size_t>(-1);
			u8 resident_self_link_entry_loads = 0;
			GprLinkSignature gpr_link_signature{};
			size_t compatible_link_entry_offset = static_cast<size_t>(-1);
			CompatibleVtlbFastEntryOffsets compatible_vtlb_fast_entries{};
			u8 compatible_link_entry_loads = 0;
			DirectLinkSlots direct_links{};
			DirectContinuationKind direct_continuation_kind =
				DirectContinuationKind::SchedulerTestedTail;
			bool discovered_topology = false;
			bool valid = false;
			bool queued_free = false;
		};

		struct PersistentRunContext
		{
			BlockExecutor* executor = nullptr;
			CachedBlock* current_block = nullptr;
			BlockExecutionResult current_result{};
			BlockExecutionResult* final_result = nullptr;
			PersistentBoundaryCallback boundary_callback = nullptr;
			PersistentPreparedCallback prepared_callback = nullptr;
			void* callback_userdata = nullptr;
			bool run_event_test_on_event_exit = true;
			bool failed = false;
		};

		struct LookupPage
		{
			std::array<CachedBlock*, LOOKUP_PAGE_ENTRY_COUNT> blocks{};
		};

		struct GeneratedLookupPage
		{
			std::array<const void*, LOOKUP_PAGE_ENTRY_COUNT> entry_points{};
		};

		struct IncomingLinkRecord
		{
			CachedBlock* source = nullptr;
			// EE instructions and every direct-link target are word aligned.
			// Store the two-slot index in those otherwise-zero low bits instead
			// of charging each of the 65,536 bounded records four bytes of A32
			// padding.
			u32 target_pc_and_slot = 0;

			IncomingLinkRecord() = default;
			IncomingLinkRecord(CachedBlock* source_, u32 target_pc_,
				u8 slot_index_)
				: source(source_)
				, target_pc_and_slot(
					(target_pc_ & ~u32{3}) | (slot_index_ & u8{3}))
			{
			}
			u32 TargetPc() const { return target_pc_and_slot & ~u32{3}; }
			u8 SlotIndex() const
			{
				return static_cast<u8>(target_pc_and_slot & u32{3});
			}
		};

		struct BlockRecord
		{
			CachedBlock* block = nullptr;
			u32 start_pc = 0;
		};
#if UINTPTR_MAX == UINT32_MAX
		static_assert(sizeof(IncomingLinkRecord) == 8);
		static_assert(sizeof(BlockRecord) == 8);
#endif

		struct RamSourceRecord
		{
			CachedBlock* block = nullptr;
			u32 serial = 0;
		};

		struct PersistentRegionContinuation
		{
			VitaA32::CodeBuffer code;
			u32 start_pc = 0;
			u32 instruction_count = 0;
			u32 inherited_raw_cycles = 0;
			u32 source_generation = 0;
			u32 scaled_cycles = 0;
			s8 ee_cycle_rate = 0;
			u8 cp0_config_cycle_shift = 0;
			bool scheduler_test_at_end = true;
			bool valid = false;
		};

		static u32 LookupPageIndex(u32 start_pc);
		static u32 LookupEntryIndex(u32 start_pc);
		bool EnsureLookupDirectory(bool discovered_topology);
		LookupPage* GetLookupPage(u32 start_pc, bool allocate,
			bool discovered_topology);
		bool EnsureGeneratedLookupDirectory();
		GeneratedLookupPage* GetGeneratedLookupPage(u32 start_pc, bool allocate);
		void RegisterBlockLookup(CachedBlock& block);
		void UnregisterBlockLookup(CachedBlock& block);
		void ReleaseLookupPages();
		void ReleaseGeneratedLookupPages();
		s32 LastBlockRecordIndex(u32 pc) const;
		bool RegisterBlockRecord(CachedBlock& block);
		void UnregisterBlockRecord(CachedBlock& block);
		void ClearBlockRecords();
		bool CaptureRamSourceFragments(CachedBlock& block);
		void RegisterRamSource(CachedBlock& block);
		void UnregisterRamSource(const CachedBlock& block);
		bool RamSourceChunkHasLiveOwner(u32 chunk_index,
			const CachedBlock& removed_block) const;
		void RefreshRamSourceChunksForBlockRemoval(const CachedBlock& block);
		void ClearRamSourcePages();
		static void InvalidateRamSourceRangeThunk(
			void* context, u32 backing_start, u32 size);
		CachedBlock* FindRecordedBlockByStartPc(u32 start_pc,
			u32 instruction_count, bool match_instruction_count,
			bool discovered_topology, bool validate_source_words = true);
		void RememberFreeCacheEntry(CachedBlock& block);
		CachedBlock* TakeFreeCacheEntry();
		void AdvanceSourceGeneration();
		void ClearPersistentRegionContinuations();
		void InvalidateCachedBlock(CachedBlock& block);
		DirectLinkSlot* GetRecordedDirectLink(IncomingLinkRecord& record);
		s32 LastIncomingLinkIndex(u32 target_pc) const;
		void ClearIncomingLinks();
		void RegisterIncomingLinks(CachedBlock& block);
		void UnregisterIncomingLinks(CachedBlock& block);
		bool CachedBlockHasDirectSourceSpan(const CachedBlock& block) const;
		bool CachedBlockSourceMatches(const CachedBlock& block) const;
		u32 RetireStaleOverlappingBlocks(u32 start_pc, u32 instruction_count,
			bool discovered_topology);
		bool ValidateCachedBlock(CachedBlock& block, bool validate_source_words = true);
		CachedBlock* FindLookupBlockByStartPc(u32 start_pc,
			bool discovered_topology);
		bool FindCachedBlock(u32 start_pc, u32 instruction_count, CachedBlock** block,
			bool* lookup_hit, bool match_code_budget_source = false);
		CachedBlock* FindLinkTargetByStartPc(u32 start_pc,
			bool discovered_topology, bool validate_source_words = true);
		void ResolveAdjacentSplitDependency(u32 start_pc, u32 instruction_count,
			bool discovered_topology,
			u32* dependency_start_pc, u32* dependency_instruction_count,
			u32* dependency_charged_cycles_before) const;
		CachedBlock* AllocateCacheEntry();
		bool EnsureCodeCache();
		void ReleaseCodeCache();
		enum class RegionCodeSlotState : u8
		{
			Free,
			Reserved,
			Committed,
			Retired,
		};
		void ResetRegionCodeArena();
		bool EnsurePersistentRegionContinuationCodeCache();
		void ReleasePersistentRegionContinuationCodeCache();
		bool LocateRegionCodeSlot(size_t slice_offset, bool* probe,
			size_t* slot_index, size_t* slot_capacity) const;
		u8* AllocateCodeSlice(size_t capacity, size_t* slice_offset);
		void CommitCodeSlice(size_t slice_offset, size_t code_size);
		void RewindCodeCache(size_t slice_offset);
		u32 ResetForCachePressure();
		u32 InvalidateRangeInternal(u32 start_pc, u32 instruction_count,
			const bool* discovered_topology);
		bool CompileIntoCacheEntry(CachedBlock& block, u32 start_pc, u32 instruction_count,
			u32* scaled_cycles, bool allow_code_budget_split = false,
			u32 dependency_start_pc = 0, u32 dependency_instruction_count = 0,
			u32 dependency_charged_cycles_before = 0,
			bool pcsx2_short_split = false,
			bool discovered_topology = false);
		bool AnalyzeGprLinkSignature(u32 start_pc, u32 instruction_count,
			GprLinkSignature* signature) const;
		bool PrepareCompiledBlockAtPc(u32 start_pc, CachedBlock** block, BlockExecutionResult* result);
		bool RunCachedBlock(CachedBlock& block, bool run_event_test_on_event_exit, BlockExecutionResult* result);
		bool EnsurePersistentDispatcher();
		static const void* PersistentDispatchThunk(u32 exit_value, void* userdata);
		const void* LinkedEntryPoint(const CachedBlock& block) const;
		const void* ResidentSelfLinkEntryPoint(const CachedBlock& block) const;
		const void* CompatibleLinkEntryPoint(const CachedBlock& block) const;
		const void* CompatibleVtlbFastEntryPoint(const CachedBlock& block,
			CompatibleVtlbGuardKind kind) const;
		bool PatchDirectLink(CachedBlock& block, DirectLinkSlot& link, CachedBlock* target);
		void PatchIncomingLinks(CachedBlock& target);
		bool UnlinkIncomingLinks(u32 target_pc,
			const bool* discovered_topology = nullptr);
		void RelinkDirectLinks();
		bool IsPersistentDispatchBarrier(u32 pc) const;
		const PersistentGeneratedEntry* FindPersistentGeneratedEntryRecord(
			u32 pc) const;
		const void* FindPersistentGeneratedEntry(u32 pc) const;
		const void* PublishedGeneratedEntry(const CachedBlock& block) const;
		bool SetCanonicalPersistentDispatchBarrier(u32 canonical_pc, bool enabled);
#if defined(VITASX2_QEMU_VALIDATION)
		void RecordPersistentExit(BlockExitKind exit);
#endif

		std::vector<std::unique_ptr<CachedBlock>> m_cache;
		CachedBlock* m_free_cache_head = nullptr;
		std::vector<BlockRecord> m_block_records;
		std::vector<IncomingLinkRecord> m_incoming_links;
		std::array<std::vector<RamSourceRecord>, RAM_SOURCE_PAGE_COUNT>
			m_ram_source_pages;
		std::array<u32, RAM_SOURCE_PAGE_COUNT> m_ram_source_page_live_counts{};
		std::array<u8, RAM_WRITE_GUARD_PAGE_COUNT>
			m_ram_source_page_live_flags{};
		std::array<u8, RAM_SOURCE_CHUNK_LIVE_BIT_BYTES>
			m_ram_source_chunk_live_bits{};
		u32 m_next_source_serial = 1;
		// Changes whenever a live tier-zero source owner is retired, including a
		// whole-cache reset. A separately allocated region cache can compare one
		// word at entry instead of rereading every immutable guest opcode.
		u32 m_source_generation = 1;
		// Explicit trace windows and PCSX2-discovered BaseBlocks can have the same
		// guest PC but different spans and scheduler tails. Keep their metadata
		// lookups independent; only discovered blocks enter generated dispatch.
		std::array<LookupPage**, 2> m_lookup_pages{};
		GeneratedLookupPage** m_generated_lookup_pages = nullptr;
		GeneratedLookupPage** m_active_generated_lookup_pages = nullptr;
		VitaA32::CodeBuffer m_persistent_dispatch_code;
		const void* m_persistent_dispatch_entry = nullptr;
		const void* m_persistent_direct_exit = nullptr;
		const void* m_persistent_scheduler_elided_direct_exit = nullptr;
		const void* m_persistent_scheduler_elided_redispatch = nullptr;
		const void* m_persistent_event_exit = nullptr;
		const void* m_persistent_retained_wait_event_exit = nullptr;
		const void* m_persistent_region_generated_failure_exit = nullptr;
		bool m_persistent_generated_failure = false;
		u32 m_persistent_region_resume_active = 0;
		u32 m_persistent_region_resume_requires_boundary = 0;
		// Four PCSX2 lifecycle owners, 32 region entries, four admission probes,
		// and replacement headroom. SetPersistentDispatchBarriers() publishes a new
		// owner before removing an evicted one, so the table must exceed the desired
		// steady-state union rather than merely equal it.
		static constexpr size_t MAX_PERSISTENT_DISPATCH_BARRIERS = 48;
		std::array<u32, MAX_PERSISTENT_DISPATCH_BARRIERS>
			m_persistent_dispatch_barriers{};
		u8 m_persistent_dispatch_barrier_count = 0;
		// Thirty-two compiled regions plus four transient first-class admission
		// probes. Keep modest replacement headroom for transactional publication.
		// RegionRuntime publishes up to 32 region owners and 32 temporary latch
		// probes in one outer-boundary transaction. This directory is metadata;
		// generated code remains in the bounded fixed-slot executable arena.
		static constexpr size_t MAX_PERSISTENT_GENERATED_ENTRIES = 64;
		std::array<PersistentGeneratedEntry,
			MAX_PERSISTENT_GENERATED_ENTRIES> m_persistent_generated_entries{};
		u8 m_persistent_generated_entry_count = 0;
		std::array<PersistentRegionContinuation,
			PERSISTENT_REGION_CONTINUATION_SLOT_COUNT>
			m_persistent_region_continuations{};
#if defined(VITASX2_QEMU_VALIDATION)
		u64 m_persistent_generated_publication_patches = 0;
#endif
		u8* m_code_cache = nullptr;
		size_t m_code_cache_capacity = 0;
		size_t m_code_cache_used = 0;
		size_t m_region_code_arena_offset = 0;
		std::array<RegionCodeSlotState, REGION_CODE_SLOT_COUNT>
			m_region_code_slots{};
		std::array<RegionCodeSlotState, REGION_PROBE_CODE_SLOT_COUNT>
			m_region_probe_code_slots{};
		std::array<RegionCodeSlotState,
			PERSISTENT_REGION_CONTINUATION_SLOT_COUNT>
			m_persistent_region_continuation_code_slots{};
		u8* m_persistent_region_continuation_code_cache = nullptr;
		u32 m_code_cache_resets = 0;
		bool m_direct_linking_enabled = true;
		bool m_persistent_dispatch_enabled = false;
#if defined(VITASX2_QEMU_VALIDATION)
		bool m_compatible_gpr_dirty_carry_enabled = true;
		bool m_compatible_scheduler_carry_enabled = true;
		bool m_compatible_vtlb_pointer_carry_enabled = true;
		bool m_compatible_vtlb_host_reclaim_enabled = true;
		bool m_compatible_predicate_carry_enabled = true;
		bool m_compatible_likely_taken_suffix_enabled = true;
		bool m_compatible_predicate_entry_variant_enabled = true;
		bool m_embedded_compatible_continuation_enabled = true;
		bool m_fused_direct_event_link_enabled = true;
		bool m_combined_compatible_taken_event_enabled = true;
		bool m_compatible_vtlb_write_guard_hoist_enabled = true;
		bool m_compatible_vtlb_read_guard_hoist_enabled = true;
		bool m_single_block_gpr_link_enabled = true;
		bool m_reciprocal_jump_gpr_link_enabled = true;
		bool m_three_block_gpr_link_enabled = true;
		bool m_vtlb_linked_entry_pc_publication_enabled = false;
		bool m_direct_link_rejection_profile_enabled = false;
		VitaA32EeLinkRejectionProfile m_direct_link_rejection_profile{};
		std::unordered_map<u64,
			std::array<u64, static_cast<u32>(VitaA32EeLinkRejectionKind::Count)>>
			m_direct_link_rejection_edges;
#endif
	};
} // namespace VitaEE
