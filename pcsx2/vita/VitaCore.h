// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

// Vita CPU provider selection. Bring-up executables can force interpreter or
// A32 EE mode directly; VMManager.cpp::UpdateCPUImplementations() uses the
// configured selector.
void VitaSelectInterpreterCpuProviders();
void VitaSelectA32EeCpuProviders();
void VitaSelectA32IopCpuProviders();
void VitaSelectA32EeIopCpuProviders();
void VitaSelectConfiguredCpuProviders();
void VitaNotifyIopPcDiscontinuity();

extern bool g_vita_a32_iop_private_event_entry_available;
extern bool g_vita_a32_iop_private_wait_resume_entry_available;
extern bool g_vita_a32_iop_private_scheduler_resume_entry_available;
extern bool g_vita_a32_iop_private_scheduler_prediction_entry_available;
extern bool g_vita_a32_iop_private_scheduler_dispatch_cache_entry_available;
#if defined(__arm__)
extern "C" void VitaCpuEventTestSharedPrivate();
bool VitaCpuEventTestSharedPrivateSupported();
extern "C" s32 VitaIopA32ExecuteProviderTimeslicePrivate(
	void* context, s32 ee_cycles);
extern "C" s32 VitaIopA32ExecuteProviderSchedulerDirectResumePrivate(
	void* context, s32 ee_cycles);
extern "C" s32 VitaIopA32ExecuteProviderSchedulerPredictedResumePrivate(
	void* context, s32 ee_cycles);
extern "C" s32 VitaIopA32ExecuteProviderSchedulerDispatchCachedResumePrivate(
	void* context, s32 ee_cycles);
extern "C" s32 VitaIopA32ExecuteProviderWaitResumePrivate(
	void* context, s32 ee_cycles);
extern "C" s32 VitaIopA32ExecuteProviderWaitResumeUnconditionalPrivate(
	void* context, s32 ee_cycles);
extern "C" s32 VitaIopA32ExecuteProviderWaitResumePollPrivate(
	void* context, s32 ee_cycles);
extern "C" s32 VitaIopA32ExecuteProviderWaitResumeConditionalPrivate(
	void* context, s32 ee_cycles);
extern "C" s32 VitaIopA32ExecuteProviderWaitResumeUnconditionalNormalPrivate(
	void* context, s32 ee_cycles);
extern "C" s32 VitaIopA32ExecuteProviderWaitResumeUnconditionalPs1Private(
	void* context, s32 ee_cycles);
extern "C" s32 VitaIopA32ExecuteProviderWaitResumeUnconditionalNoLinkNormalPrivate(
	void* context, s32 ee_cycles);
extern "C" s32 VitaIopA32ExecuteProviderWaitResumeUnconditionalNoLinkPs1Private(
	void* context, s32 ee_cycles);
extern "C" s32 VitaIopA32ExecuteProviderWaitResumePollNormalPrivate(
	void* context, s32 ee_cycles);
extern "C" s32 VitaIopA32ExecuteProviderWaitResumePollPs1Private(
	void* context, s32 ee_cycles);
extern "C" s32 VitaIopA32ExecuteProviderWaitResumeConditionalNormalPrivate(
	void* context, s32 ee_cycles);
extern "C" s32 VitaIopA32ExecuteProviderWaitResumeConditionalPs1Private(
	void* context, s32 ee_cycles);
void VitaSetA32IopWaitResumeEventEntry(uptr context, uptr target,
	bool retained_unconditional_normal_wait);
void VitaSetA32IopSchedulerDirectEventContext(uptr context);
extern bool g_vita_a32_iop_retained_unconditional_normal_wait;
#if defined(VITASX2_QEMU_VALIDATION)
extern bool g_vita_a32_iop_retained_wait_coalescing_validation_enabled;
extern bool g_vita_a32_iop_deadline_gate_validation_enabled;
extern bool g_vita_ee_interleave_scheduler_validation_enabled;
extern u64 g_vita_ee_full_scheduler_validation_entries;
extern u64 g_vita_ee_iop_only_scheduler_validation_entries;
extern bool g_vita_joint_wait_horizon_validation_enabled;
#endif
inline __attribute__((always_inline))
bool VitaA32IopRetainedWaitCoalescingActive()
{
#if defined(VITASX2_QEMU_VALIDATION)
	return g_vita_a32_iop_retained_wait_coalescing_validation_enabled &&
		g_vita_a32_iop_retained_unconditional_normal_wait;
#else
	return g_vita_a32_iop_retained_unconditional_normal_wait;
#endif
}
namespace VitaIOP
{
	bool VitaIopA32PrivateTimesliceEntrySupported();
	bool VitaIopA32PrivateWaitResumeEntrySupported();
	bool VitaIopA32PrivateSchedulerResumeEntrySupported();
	bool VitaIopA32PrivateSchedulerPredictionEntrySupported();
	bool VitaIopA32PrivateSchedulerDispatchCacheEntrySupported();
}

struct alignas(8) VitaA32IopEventEntry
{
	uptr context = 0;
	uptr target = 0;
};
static_assert(sizeof(VitaA32IopEventEntry) == 8);
extern VitaA32IopEventEntry g_vita_a32_iop_event_entry;

inline __attribute__((always_inline)) s32 VitaExecuteA32IopTimesliceFromEeEvent(
	s32 ee_cycles)
{
	register uptr context_and_result asm("r0");
	register s32 cycles asm("r1") = ee_cycles;
	register const VitaA32IopEventEntry* entry asm("r2") =
		&g_vita_a32_iop_event_entry;
	asm volatile(
		"ldmia %[entry], {r0, r12}\n\t"
		"blx r12"
		: "=r"(context_and_result), "+r"(cycles), [entry] "+r"(entry)
		:
		: "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12",
		  // The private-body translation unit reserves d8-d15, generated IOP
		  // code uses only q0, and nested AAPCS helpers preserve d8-d15.
		  // Describe the ordinary caller-clobbered VFP bank rather than forcing
		  // _cpuEventTest_Shared() to save 128 bytes at every scheduler seam.
		  "lr", "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
		  "cc", "memory");
	return static_cast<s32>(context_and_result);
}

inline __attribute__((always_inline)) void
VitaRunCpuEventTestSharedFromOwnedEeFrame()
{
	asm volatile(
		"bl VitaCpuEventTestSharedPrivate"
		:
		:
		: "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9",
		  "r10", "r11", "r12", "lr", "d0", "d1", "d2", "d3", "d4", "d5",
		  "d6", "d7", "cc", "memory");
}
#endif

#if defined(VITASX2_QEMU_VALIDATION)
extern bool g_vita_a32_iop_private_event_entry_enabled;
extern bool g_vita_a32_iop_wait_resume_event_entry_enabled;
extern bool g_vita_a32_iop_scheduler_resume_event_entry_enabled;
extern bool g_vita_a32_iop_scheduler_prediction_event_entry_enabled;
extern bool g_vita_a32_iop_scheduler_dispatch_cache_event_entry_enabled;
extern u64 g_vita_a32_iop_private_event_entries;
#endif

struct VitaA32EeProviderStats
{
	// Compile-time code-generation totals. These count each successfully
	// published A32 block once; compiled_blocks below remains the existing
	// runtime provider-boundary count.
	u64 generated_blocks = 0;
	u64 generated_host_instructions = 0;
	u64 generated_host_load_instructions = 0;
	u64 generated_host_store_instructions = 0;
	u64 generated_helper_call_instructions = 0;
	u64 generated_state_load_instructions = 0;
	u64 generated_state_store_instructions = 0;
	// Guest-family mix of the successfully published blocks above.  This is
	// compile-seam evidence only: normal generated execution performs no
	// telemetry stores.  Families follow VitaEeBlockCompiler.cpp's owning
	// top-level EmitOpcode()/branch dispatch.
	u64 generated_guest_instructions = 0;
	u64 generated_integer_instructions = 0;
	u64 generated_branch_instructions = 0;
	u64 generated_gpr_load_instructions = 0;
	u64 generated_gpr_store_instructions = 0;
	u64 generated_mmi_instructions = 0;
	u64 generated_cop0_instructions = 0;
	u64 generated_cop1_instructions = 0;
	u64 generated_cop2_instructions = 0;
	u64 generated_cop2_runtime_noop_instructions = 0;
	u64 generated_vu0_acc_cache_writes = 0;
	u64 generated_vu0_acc_cache_hits = 0;
	u64 generated_vu0_acc_cache_flushes = 0;
	u64 generated_other_instructions = 0;
	u32 generated_poll_call_wait_blocks = 0;
	u32 generated_multi_range_poll_call_wait_blocks = 0;
	u32 generated_poll_call_wait_additional_ram_watches = 0;
	u32 generated_two_predicate_wait_blocks = 0;
	u32 two_predicate_wait_fast_forwards = 0;
	u32 largest_generated_block_pc = 0;
	u32 largest_generated_block_guest_instructions = 0;
	u32 largest_generated_block_host_instructions = 0;
	u32 largest_generated_block_helper_calls = 0;
	u32 largest_generated_block_state_loads = 0;
	u32 largest_generated_block_state_stores = 0;
	u32 compiled_blocks = 0;
	u32 compiled_instructions = 0;
	u32 interpreter_steps = 0;
	u32 direct_exits = 0;
	u32 event_exits = 0;
	u32 failed_blocks = 0;
	u32 cache_hits = 0;
	u32 cache_misses = 0;
	u32 lookup_hits = 0;
	u32 fast_dispatch_hits = 0;
	u32 invalidated_blocks = 0;
	// Snapshot-only cache-pressure evidence. VitaGetA32EeProviderStats() reads
	// these gauges at the existing correlated-performance boundary; generated
	// execution and block publication perform no additional telemetry work.
	u32 code_cache_resets = 0;
	u32 code_cache_block_records = 0;
	u32 code_cache_slots = 0;
	u64 code_cache_used = 0;
	u64 code_cache_capacity = 0;
	// Bounded product telemetry. Candidates are derived from tests-refusals so
	// the hot accepted path performs only one 32-bit increment on ARMv7.
	u32 in_frame_event_tests = 0;
	u32 in_frame_event_resume_candidates = 0;
	u32 in_frame_event_resume_refusals = 0;
	u32 retained_unconditional_wait_events = 0;
	u32 retained_dmac_chcr_poll_events = 0;
	u32 retained_ram_wait_events = 0;
	u32 retained_ram_wait_write_exits = 0;
	u32 first_interpreter_pc = 0;
	u32 first_interpreter_opcode = 0;
	u32 first_interpreter_reason = 0;
	u32 last_interpreter_pc = 0;
	u32 last_interpreter_opcode = 0;
	u32 last_interpreter_reason = 0;
	u32 scan_unsupported_fallbacks = 0;
	u32 scan_boundary_fallbacks = 0;
	u32 exact_trace_branch_likely_fallbacks = 0;
	u32 execute_failed_fallbacks = 0;
	u32 interpreter_path_fallbacks = 0;
};

struct VitaA32EeGeneratedGuestMix
{
	u32 instructions = 0;
	u32 integer = 0;
	u32 branch = 0;
	u32 gpr_load = 0;
	u32 gpr_store = 0;
	u32 mmi = 0;
	u32 cop0 = 0;
	u32 cop1 = 0;
	u32 cop2 = 0;
	u32 cop2_runtime_noop = 0;
	u32 other = 0;
};

// Provenance for the PCSX2-proven EE wait which entered the shared scheduler.
// RAM ranges use physical offsets into eeMem->Main so aliases compare equal.
// A zero range count deliberately fails closed for RAM-poll wait kinds.
enum class VitaA32EeWaitSchedulerOrigin : u32
{
	None = 0,
	GenericRamLoop = 1,
	PollCallRamLoop = 2,
	TwoPredicateRamLoop = 3,
	RetainedUnconditionalLoop = 4,
	GsCsrVsintLoop = 5,
	DmacChcrStrPollLoop = 6,
	IntcVblankStartAndRamLoop = 7,
};

inline constexpr u32 VITA_A32_EE_WAIT_RAM_RANGE_CAPACITY = 3;

struct VitaA32EeWaitSchedulerCertificate
{
	VitaA32EeWaitSchedulerOrigin origin =
		VitaA32EeWaitSchedulerOrigin::None;
	u8 ram_range_count = 0;
	u8 ram_write_observed = 0;
	u8 reserved[2]{};
	union
	{
		struct
		{
			u32 ram_offset[VITA_A32_EE_WAIT_RAM_RANGE_CAPACITY];
			u32 ram_size[VITA_A32_EE_WAIT_RAM_RANGE_CAPACITY];
		};
		// DMAC CHCR.STR polls do not watch EE RAM. These fields retain the
		// complete structural loop contract across scheduler events without
		// increasing this hot certificate's size.
		struct
		{
			u32 mmio_fallthrough_pc;
			u32 mmio_block_cycles;
			u32 mmio_packed_poll;
			u32 mmio_reserved;
		};
	};
	union
	{
		struct
		{
			u32 poll_packed_cycles;
			u32 poll_leaf_pc;
			u32 poll_return_pc;
			u32 poll_call_pc;
		};
		struct
		{
			u32 predicate_prefix_cycles;
			u32 predicate_tail_cycles;
			u32 predicate_loop_pc;
			u32 predicate_tail_pc;
		};
	};
};
static_assert(sizeof(VitaA32EeWaitSchedulerCertificate) == 48);

void VitaPublishA32EeWaitSchedulerOrigin(
	VitaA32EeWaitSchedulerOrigin origin);
void VitaPublishA32EeRamWaitSchedulerCertificate(
	VitaA32EeWaitSchedulerOrigin origin,
	u32 guest_address_0, u32 size_0,
	u32 guest_address_1 = 0, u32 size_1 = 0);
void VitaPublishA32EePollCallWaitSchedulerCertificate(
	u32 guest_address, u32 packed_cycles,
	u32 leaf_pc, u32 return_pc, u32 call_pc,
	u32 guest_address_1 = 0, u32 size_1 = 0,
	u32 guest_address_2 = 0, u32 size_2 = 0);
void VitaPublishA32EeTwoPredicateWaitSchedulerCertificate(
	u32 guest_address_0, u32 guest_address_1,
	u32 prefix_cycles, u32 tail_cycles,
	u32 loop_pc, u32 tail_pc);
void VitaPublishA32EeIntcVblankStartAndRamWaitSchedulerCertificate(
	u32 ram_address, u32 prefix_cycles, u32 tail_cycles,
	u32 loop_pc, u32 tail_pc);
void VitaPublishA32EeDmacChcrWaitSchedulerCertificate(
	u32 fallthrough_pc, u32 block_cycles, u32 packed_poll);
void VitaRepublishA32EeWaitSchedulerCertificate(
	const VitaA32EeWaitSchedulerCertificate& certificate);
const VitaA32EeWaitSchedulerCertificate*
VitaConsumeA32EeWaitSchedulerCertificate();
void VitaFinishA32EeWaitSchedulerCertificate();
bool VitaWasA32EeWaitSchedulerRamWriteObserved();

#if defined(VITASX2_QEMU_VALIDATION)
static constexpr u32 VITA_A32_EE_LINK_REJECTION_EDGE_COUNT = 16;

enum class VitaA32EeLinkRejectionKind : u32
{
	UnrecordedEdge,
	TargetNotCompiled,
	SourceSignatureMissing,
	TargetSignatureMissing,
	SignatureMismatch,
	CompatibleEntryMissing,
	EmbeddedSourceMismatch,
	UnexpectedFallback,
	Count,
};

struct VitaA32EeLinkRejectionEdge
{
	u32 source_pc = 0;
	u32 target_pc = 0;
	VitaA32EeLinkRejectionKind kind = VitaA32EeLinkRejectionKind::UnrecordedEdge;
	u64 exits = 0;
};

struct VitaA32EeLinkRejectionProfile
{
	u64 persistent_boundaries = 0;
	u64 event_exits = 0;
	u64 direct_exits = 0;
	u64 kinds[static_cast<u32>(VitaA32EeLinkRejectionKind::Count)]{};
	u32 edge_count = 0;
	VitaA32EeLinkRejectionEdge edges[VITA_A32_EE_LINK_REJECTION_EDGE_COUNT]{};
};
#endif

enum class VitaA32EeFallbackReason : u32
{
	None = 0,
	ScanUnsupportedOpcode = 1,
	ScanBoundary = 2,
	ExactTraceBranchLikely = 3,
	ExecuteFailed = 4,
	InterpreterPath = 5,
};

const char* VitaA32EeFallbackReasonName(VitaA32EeFallbackReason reason);
void VitaResetA32EeProviderStats();
VitaA32EeProviderStats VitaGetA32EeProviderStats();
void VitaRecordA32EeGeneratedCode(u32 start_pc,
	const VitaA32EeGeneratedGuestMix& guest_mix, u64 host_instructions,
	u64 host_load_instructions, u64 host_store_instructions,
	u64 helper_call_instructions, u64 state_load_instructions,
	u64 state_store_instructions, bool poll_call_wait_loop,
	u32 poll_call_wait_additional_ram_watches,
	bool two_predicate_wait_loop, u32 vu0_acc_cache_writes,
	u32 vu0_acc_cache_hits, u32 vu0_acc_cache_flushes);
void VitaRecordA32EeTwoPredicateWaitLoopFastForward();
void VitaRequestA32EeCacheReset();
// Normal product execution enables PCSX2-style DispatcherEvent fallthrough.
// Validation modes leave it disabled unless they explicitly own the event
// callback contract and its natural-boundary observations.
void VitaSetA32EeInFrameEventResumeEnabled(bool enabled);
// Notify the active EE A32 provider after a C++ helper or device has written
// directly through an eeMem->Main host pointer. Returns the number of cached
// translations retired by the write.
u32 VitaNotifyA32EeRamWrite(const void* host_address, u32 size);
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
enum class VitaA32EeTraceLimitStopCondition : u32
{
	None = 0,
	CoreEventTrace = 1,
	MachineCheckpointTrace = 2,
};

// Stop an uninstrumented persistent EE chain only after its current natural
// block boundary observes the selected bounded cross-core trace limit.
void VitaSetA32EeTraceLimitStopCondition(VitaA32EeTraceLimitStopCondition condition);
#endif

#if defined(VITASX2_PRODUCT_BOOT_VALIDATION)
using VitaVSyncProgressCallback = void (*)();

// Observe PCSX2's CPU-thread VSync seam without moving device/event work out of
// its owner. The product validation uses this only for sparse durable progress
// receipts; normal Vita builds carry no callback branch.
void VitaSetVSyncProgressCallback(VitaVSyncProgressCallback callback);
#endif

#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
// Unlike cache/reset-scoped provider telemetry, these fallback sentinels span
// the complete validation session, including the two ELF-entry cache resets.
void VitaResetA32EeSessionFallbackStats();
VitaA32EeProviderStats VitaGetA32EeSessionFallbackStats();
#endif

#if defined(VITASX2_QEMU_VALIDATION)
// Exercise the product post-scheduler DMAC CHCR poll iteration without synthesizing
// a complete persistent-dispatch frame. Return values match the internal
// Direct/RepeatEvent/UncertifiedEvent decision in VitaCpuProviders.cpp.
u32 VitaRunA32EeDmacChcrPollIterationAfterEventForValidation(
	u32 loop_pc, const VitaA32EeWaitSchedulerCertificate& certificate);
u32 VitaRunA32EeRamWaitIterationAfterEventForValidation(
	u32 wait_pc, const VitaA32EeWaitSchedulerCertificate& certificate,
	bool write_observed);
void VitaSetA32EeLinkRejectionProfileEnabled(bool enabled);
void VitaSetA32EePersistentBoundaryLimit(u64 limit);
bool VitaDidA32EePersistentBoundaryHitLimit();
VitaA32EeLinkRejectionProfile VitaGetA32EeLinkRejectionProfile();
#endif

struct VitaA32IopProviderStats
{
	u32 compiled_blocks = 0;
	u32 compiled_instructions = 0;
	u32 native_instructions = 0;
	u32 helper_instructions = 0;
	u32 executed_blocks = 0;
	u32 direct_exits = 0;
	u32 failed_blocks = 0;
	u32 interpreter_blocks = 0;
	u32 cache_hits = 0;
	u32 cache_misses = 0;
	u32 lookup_hits = 0;
	u32 fast_dispatch_hits = 0;
	u32 invalidated_blocks = 0;
	u32 code_cache_resets = 0;
	u32 code_cache_block_records = 0;
	u32 code_cache_slots = 0;
	u32 semantic_block_descriptors = 0;
	u64 code_cache_used = 0;
	u64 code_cache_capacity = 0;
	u32 isolate_mode_switches = 0;
	u32 first_interpreter_pc = 0;
	u32 first_interpreter_opcode = 0;
	u32 last_interpreter_pc = 0;
	u32 last_interpreter_opcode = 0;
	u64 wait_loop_fast_forwards = 0;
	u64 wait_loop_iop_cycles = 0;
	u64 wait_loop_dispatches_elided = 0;
	u64 wait_loop_block_entries_elided = 0;
	u64 poll_call_wait_loop_fast_forwards = 0;
	u64 poll_call_wait_loop_dispatches_elided = 0;
#if defined(VITASX2_QEMU_VALIDATION)
	u64 hot_dispatch_cache_hits = 0;
	u64 hot_dispatch_cache_misses = 0;
	u64 hot_dispatch_cache_way_probes = 0;
	u64 hot_dispatch_cache_64_set_hits = 0;
	u64 hot_dispatch_cache_64_set_misses = 0;
	u64 hot_dispatch_cache_64_set_way_probes = 0;
	u64 hot_dispatch_cache_way_probes_removed = 0;
	u64 scheduler_direct_resume_candidates = 0;
	u64 scheduler_direct_resume_installs = 0;
	u64 scheduler_direct_resume_attempts = 0;
	u64 scheduler_direct_resume_hits = 0;
	u64 scheduler_direct_resume_misses = 0;
	u64 scheduler_direct_resume_no_target = 0;
	u64 scheduler_direct_resume_target_mismatch = 0;
	u64 scheduler_direct_resume_hot_lookups_removed = 0;
	u64 scheduler_direct_event_entries = 0;
	u64 scheduler_direct_event_forwards = 0;
	u64 scheduler_direct_event_fallbacks = 0;
	u64 scheduler_direct_event_remainders = 0;
	u64 scheduler_direct_event_installs = 0;
	u64 scheduler_direct_event_clears = 0;
	u64 scheduler_prediction_attempts = 0;
	u64 scheduler_prediction_hits = 0;
	u64 scheduler_prediction_misses = 0;
	u64 scheduler_prediction_two_way_hits = 0;
	u64 scheduler_prediction_four_way_hits = 0;
	u64 scheduler_prediction_forwards = 0;
	u64 scheduler_prediction_fallbacks = 0;
	u64 scheduler_prediction_remainders = 0;
	u64 scheduler_dispatch_cache_attempts = 0;
	u64 scheduler_dispatch_cache_hits = 0;
	u64 scheduler_dispatch_cache_misses = 0;
	u64 scheduler_dispatch_cache_forwards = 0;
	u64 scheduler_dispatch_cache_fallbacks = 0;
	u64 scheduler_dispatch_cache_remainders = 0;
	u64 scheduler_dispatch_cache_installs = 0;
	u64 hot_dispatch_trusted_raw_hits = 0;
	u64 hot_dispatch_owned_hits = 0;
	u64 hot_dispatch_stale_guard_instructions_removed = 0;
	u32 hot_dispatch_hit_pc_count = 0;
	u32 hot_dispatch_hit_pcs[16]{};
	u64 hot_dispatch_hit_pc_hits[16]{};
	u32 interpreter_fallback_pc_count = 0;
	u32 interpreter_fallback_pcs[16]{};
	u32 interpreter_fallback_owner_pcs[16]{};
	u32 interpreter_fallback_owner_opcodes[16]{};
	u32 interpreter_fallback_instruction_counts[16]{};
	u64 interpreter_fallback_source_hashes[16]{};
	u64 interpreter_fallback_hits[16]{};
	u64 wait_resume_cache_attempts = 0;
	u64 wait_resume_cache_hits = 0;
	u64 wait_resume_cache_misses = 0;
	u64 wait_resume_event_entries = 0;
	u64 wait_resume_event_forwards = 0;
	u64 wait_resume_event_fallbacks = 0;
	u64 wait_resume_event_installs = 0;
	u64 wait_resume_event_clears = 0;
	u64 wait_resume_first_entry_owned = 0;
	u64 wait_resume_first_entry_identity_instructions_removed = 0;
	u64 wait_resume_post_event_identity_checks = 0;
	u64 wait_resume_kind_specific_entries = 0;
	u64 wait_resume_kind_instructions_removed = 0;
	u64 wait_resume_clock_specific_entries = 0;
	u64 wait_resume_clock_instructions_removed = 0;
	u64 wait_resume_no_link_specific_entries = 0;
	u64 wait_resume_no_link_instructions_removed = 0;
	u64 wait_resume_descriptor_forwards = 0;
	u64 wait_resume_unconditional_forwards = 0;
	u64 wait_resume_poll_forwards = 0;
	u64 wait_resume_conditional_forwards = 0;
	u64 direct_budget_exit_provider_entries = 0;
	u64 constant_cycle_budget_provider_entries = 0;
	u64 validation_calls = 0;
	u64 validation_words = 0;
	u64 raw_validation_calls = 0;
	u64 raw_validation_words = 0;
	u64 translated_validation_words = 0;
	u64 wait_loop_configuration_checks = 0;
	u64 trusted_source_hits = 0;
	u64 trusted_source_audit_words = 0;
	u64 trusted_source_audit_failures = 0;
	u64 ram_invalidation_calls = 0;
	u64 ram_invalidation_record_visits = 0;
	u64 clock_mode_check_instructions_removed = 0;
	u64 saved_register_stack_words_removed = 0;
	u64 saved_register_frame_instructions_added = 0;
	u64 saved_register_frame_instructions_removed = 0;
	u64 batched_cycle_instructions_removed = 0;
	u64 batched_cycle_stack_words_removed = 0;
	u64 expanded_cycle_batching_provider_entries = 0;
	u64 linked_frame_bypass_entries = 0;
	u64 linked_frame_instructions_removed = 0;
	u64 linked_frame_stack_words_removed = 0;
	u64 sequential_qword_copy_fast_paths = 0;
	u64 sequential_qword_copy_instructions_removed = 0;
	u64 branch_event_candidates = 0;
	u64 branch_event_budget_positive = 0;
	u64 branch_event_tests_entered = 0;
	u64 budget_before_event_fast_exits = 0;
	u64 budget_before_event_instructions_removed = 0;
	u64 event_deadline_fast_skips = 0;
	u64 event_deadline_instructions_removed = 0;
	u64 compact_provider_dispatch_entries = 0;
	u64 compact_provider_cache_hit_entries = 0;
	u64 compact_provider_result_loads_removed = 0;
	u64 provider_runtime_stats_instructions_removed = 0;
	u64 private_dispatcher_calls = 0;
	u64 private_dispatcher_provider_entries = 0;
	u64 private_dispatcher_wait_forwards = 0;
	u64 private_dispatcher_generated_entries = 0;
	u64 private_dispatcher_fallbacks = 0;
	u64 private_dispatcher_control_transfers_removed = 0;
	u64 private_dispatcher_inlined_hot_entries = 0;
	u64 private_dispatcher_hot_path_control_transfers_removed = 0;
	u64 private_dispatcher_stack_guard_instructions_removed = 0;
	u64 private_event_entries = 0;
	u64 private_event_stack_words_removed = 0;
	u64 private_event_frame_instructions_added = 0;
	u64 private_frame_provider_entries = 0;
	u64 private_frame_stack_words_removed = 0;
	u64 private_frame_zero_scratch_entries = 0;
	u64 cached_wait_descriptor_checks = 0;
	u64 cached_wait_descriptor_forwards = 0;
	u64 cached_wait_descriptor_opcode_reads_removed = 0;
	u64 cached_wait_descriptor_memory_control_transfers_removed = 0;
	u64 cached_wait_descriptor_unconditional_checks = 0;
	u64 cached_wait_descriptor_control_transfers_removed = 0;
	u64 compiled_ps1_bios_gate_blocks = 0;
	u64 compiled_ps1_bios_gate_entries = 0;
	u64 dispatcher_ps1_bios_gate_checks_removed = 0;
	u64 dispatcher_ps1_bios_gate_instructions_removed = 0;
	u64 inline_wait_fast_forwards = 0;
	u64 inline_wait_stack_words_removed = 0;
	u64 inline_wait_control_transfers_removed = 0;
	u64 pinned_gpr_memory_ops_saved = 0;
	u64 pinned_branch_operand_moves_removed = 0;
	u64 condition_code_branch_instructions_removed = 0;
	u64 producer_branch_compare_instructions_removed = 0;
	u64 fused_ram_guard_instructions_removed = 0;
	u64 source_page_guard_instructions_removed = 0;
	u64 source_page_literal_instructions_removed = 0;
	u64 isolate_cache_guard_instructions_removed = 0;
#endif
};

#if defined(VITASX2_QEMU_VALIDATION)
static constexpr u32 VITA_A32_IOP_HOT_DISPATCH_COUNT = 16;

struct VitaA32IopHotDispatch
{
	u32 pc = 0;
	u32 opcode = 0;
	u32 instruction_count = 0;
	u64 dispatches = 0;
	u32 opcodes[16]{};
};

struct VitaA32IopHotDispatchEdge
{
	u32 source_pc = 0;
	u32 target_pc = 0;
	u64 dispatches = 0;
};

struct VitaA32IopDispatchProfile
{
	u32 count = 0;
	VitaA32IopHotDispatch entries[VITA_A32_IOP_HOT_DISPATCH_COUNT]{};
	u32 edge_count = 0;
	VitaA32IopHotDispatchEdge edges[VITA_A32_IOP_HOT_DISPATCH_COUNT]{};
};
#endif

void VitaResetA32IopProviderStats();
VitaA32IopProviderStats VitaGetA32IopProviderStats();
void VitaSetA32IopCompactProviderDispatchEnabled(bool enabled);
#if defined(VITASX2_QEMU_VALIDATION)
void VitaSetA32IopRuntimeStatsEnabled(bool enabled);
void VitaSetA32IopPrivateDispatcherEnabled(bool enabled);
void VitaSetA32IopPrivateEventEntryEnabled(bool enabled);
void VitaSetA32IopPrivateHotPathEnabled(bool enabled);
void VitaSetA32IopHotDispatchOwnershipEnabled(bool enabled);
void VitaSetA32IopCachedWaitDescriptorEnabled(bool enabled);
void VitaSetA32IopInlineWaitFastForwardEnabled(bool enabled);
void VitaSetA32IopWaitResumeCacheEnabled(bool enabled);
void VitaSetA32IopWaitResumeEventEntryEnabled(bool enabled);
void VitaSetA32IopSchedulerResumeEventEntryEnabled(bool enabled);
void VitaSetA32IopSchedulerPredictionEventEntryEnabled(bool enabled);
void VitaSetA32IopSchedulerDispatchCacheEventEntryEnabled(bool enabled);
void VitaSetA32IopWaitResumeFirstEntryOwnershipEnabled(bool enabled);
void VitaSetA32IopWaitResumeKindEntryEnabled(bool enabled);
void VitaSetA32IopWaitResumeClockEntryEnabled(bool enabled);
void VitaSetA32IopWaitResumeNoLinkEntryEnabled(bool enabled);
void VitaSetA32IopWaitResumeDescriptorSpecializationEnabled(bool enabled);
void VitaSetA32IopSchedulerPreEventWaitAdvanceEnabled(bool enabled);
VitaA32IopDispatchProfile VitaGetA32IopDispatchProfile();
void VitaRecordA32IopWaitLoopFastForward(u64 iop_cycles, u32 block_cycles);
void VitaRecordA32IopWaitLoopDispatchElision();
void VitaRecordA32IopPollCallWaitLoopDispatchElision();
#endif

// Mirrors the fast-boot ELF state that VMManager.cpp::Initialize() seeds for
// R5900.cpp::eeloadHook() in Vita bring-up executables.
void VitaClearVmBootState();
void VitaSetFastBootElfOverride(const char* elf_path);
void VitaSetFastBootDisc();

// Mirrors the PCSX2 DebugTools/EeTrace.cpp::RecordEePreInstruction hook point
// in Interpreter.cpp::execI() for Vita bring-up trace executables.
// Return true only from Cpu->Execute() flows; Cpu->Step() users should stop
// externally after recording the needed instruction count.
using VitaEePreInstructionTraceCallback = bool (*)(u32 pc, u32 opcode);
void VitaSetEePreInstructionTraceCallback(VitaEePreInstructionTraceCallback callback);
bool VitaIsEePreInstructionTraceEnabled();
bool VitaRecordEePreInstruction(u32 pc, u32 opcode);

// Optional trace-harness fast path for A32 exact-stream recording. The callback
// returns how many leading pre-instruction records in a straight-line native
// window were consumed by an outer skip gate without writing trace state.
using VitaEePreInstructionTraceWindowSkipCallback =
	u32 (*)(u32 start_pc, u32 first_opcode, u32 instruction_count);
void VitaSetEePreInstructionTraceWindowSkipCallback(
	VitaEePreInstructionTraceWindowSkipCallback callback);

enum class VitaA32EeTraceMode
{
	InstructionWindow,
	BlockBoundaryState,
};

// InstructionWindow records the executed pc/opcode stream by pre-recording
// straight-line block windows before native execution. BlockBoundaryState
// records only block starts, where the complete EE state is current and can be
// compared against PCSX2 full-state traces.
void VitaSetA32EeTraceMode(VitaA32EeTraceMode mode);

// Exact-stream trace mode for the A32 EE provider: recorded windows must
// match the executed instruction stream exactly, so branch-likely pairs are
// stepped through Interpreter.cpp::execI() (which records the delay slot only
// when it executes) instead of compiling natively. Requires initialized
// EE hardware because interpreter branches run intEventTest(). Oracle trace
// recorders enable this; synthetic provider validation keeps it off to prove
// native likely-branch blocks.
void VitaSetEeExactTraceStreams(bool enabled);

// Mirrors the PCSX2 DebugTools/IopTrace.cpp::RecordIopPreInstruction hook point
// in R3000AInterpreter.cpp::execI() for Vita bring-up trace executables.
using VitaIopPreInstructionTraceCallback = bool (*)(u32 pc, u32 opcode);
void VitaSetIopPreInstructionTraceCallback(VitaIopPreInstructionTraceCallback callback);
bool VitaIsIopPreInstructionTraceEnabled();
bool VitaRecordIopPreInstruction(u32 pc, u32 opcode);
