// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <array>
#include <cstddef>
#if defined(VITASX2_CPU_PROFILER)
#include <atomic>
#endif

namespace VitaPerformanceTelemetry
{
	// Validation executables retain their historical telemetry unless their
	// entrypoint chooses otherwise. The product sets this exactly once before
	// PCSX2 creates the EE, MTVU, and MTGS threads.
	extern bool g_enabled;

	inline bool IsEnabled()
	{
		return g_enabled;
	}

	void SetEnabledBeforeVmStart(bool enabled);

	enum class CpuStage : u8
	{
		Scheduler,
		EeExceptions,
		IopGuest,
		IopEvent,
		IopCounters,
		IopInterrupts,
		Spu2,
		Dev9,
		Usb,
		EeCounters,
		EeInterrupts,
		VuSync,
		Deadline,
		// The stages above preserve the original sparse scheduler-profiler
		// ordering.  The stages below extend one selected scheduler invocation
		// through the generated EE span which follows it.
		EeGenerated,
#if defined(VITASX2_CPU_PROFILER)
		// Host work in VitaCpuProviders.cpp's retained EE wait bridge. Keep it
		// separate from instructions executed by the generated A32 block.
		EeWaitResume,
#endif
		EeProvider,
		EeCompile,
		EeInterpreter,
#if defined(VITASX2_CPU_PROFILER)
		IopGenerated,
		IopProvider,
		IopCompile,
		Spu2Input,
		Spu2Voices,
		Spu2Core,
		Spu2Reverb,
		Spu2Output,
		IpuDecode,
		IpuIdct,
		IpuCsc,
		IpuDma,
		// R5900.cpp::TESTINT() owns these mutually exclusive EE event
		// callbacks. Keep them separate so Phase 1 profiles do not treat VIF
		// feeding, GIF feeding, and VU completion publication as one owner.
		VifDma,
		GifDma,
		VifVuFinish,
#endif
		IopInterpreter,
		Cop1,
		Cop2Vu0,
		EeHelper,
		EeMemorySlowPath,
		IopHelper,
		IopMemorySlowPath,
		Ipu,
		VifGif,
		Sif,
		Cdvd,
		Dma,
		OtherDevice,
		Diagnostics,
		Count,
#if !defined(VITASX2_CPU_PROFILER)
		// Keep diagnostic call sites source-identical while preserving the
		// normal product's original enum geometry and telemetry record size.
		IopGenerated = IopGuest,
		IopProvider = IopGuest,
		IopCompile = IopGuest,
#endif
	};

	static constexpr size_t CPU_STAGE_COUNT =
		static_cast<size_t>(CpuStage::Count);
	static constexpr u32 CPU_STAGE_SAMPLE_PERIOD = 1024;
	static constexpr size_t CPU_PROFILE_CODE_WORD_COUNT = 8;
	// R5900.h::EE_EventType is dense through VU_MTVU_BUSY. R5900.cpp keeps
	// this profiler-only storage tied to that owning enum with a static assert.
	static constexpr size_t EE_DEADLINE_EVENT_SLOT_COUNT = 21;
	// PES currently produces about 214 records per 120-VSync measurement
	// window at the 1/1024 cadence. 307 widened records retain a complete
	// ordinary window while keeping fixed interval storage below 96 KiB.
	static constexpr size_t CPU_PROFILE_INTERVAL_RING_SIZE = 307;
	static constexpr size_t CPU_PROFILE_HOT_EDGE_COUNT = 8;
	static constexpr size_t CPU_PROFILE_HOT_IOP_PC_COUNT = 8;

	enum class EeDeadlineOwner : u8
	{
		Iop,
		EeCounter,
		EeEvent,
		None,
	};

#if defined(VITASX2_CPU_PROFILER)
	enum IpuEpochOpportunityBlocker : u32
	{
		IpuEpochBlockerIopActive = 1u << 0,
		IpuEpochBlockerNoDueIpu = 1u << 1,
		IpuEpochBlockerDueNonIpu = 1u << 2,
		IpuEpochBlockerEeCounter = 1u << 3,
		IpuEpochBlockerCp0Timer = 1u << 4,
		IpuEpochBlockerVisibleException = 1u << 5,
		IpuEpochBlockerVu = 1u << 6,
		IpuEpochBlockerDmacSuspended = 1u << 7,
		IpuEpochBlockerInstantDma = 1u << 8,
	};
#endif

	enum class Spu2SyncReason : u8
	{
		Periodic,
		RegisterRead,
		RegisterWrite,
		Dma,
		Observer,
	};

	enum CpuProfileIntervalFlags : u16
	{
		CpuProfileIntervalBalanced = 1u << 0,
	};

	// Fixed-width, allocation-free record written only by CPU0.  One record
	// covers a sampled scheduler invocation and all generated EE execution up
	// to the next scheduler entry.  The PC/cycle pairs make the sample useful
	// as an edge attribution source and bind it to guest progress rather than
	// host utilization.
	struct CpuProfileIntervalRecord
	{
		u64 sequence = 0;
		u64 ee_cycle_start = 0;
		u64 ee_cycle_end = 0;
		u64 iop_cycle_start = 0;
		u64 iop_cycle_end = 0;
		u32 process_time_start_us = 0;
		u32 process_time_end_us = 0;
		u32 ee_pc_start = 0;
		u32 ee_pc_end = 0;
		u32 iop_pc_start = 0;
		u32 iop_pc_end = 0;
		std::array<u32, CPU_PROFILE_CODE_WORD_COUNT> ee_code_start{};
		std::array<u32, CPU_PROFILE_CODE_WORD_COUNT> iop_code_start{};
		u16 flags = 0;
		u16 stage_count = 0;
		std::array<u32, CPU_STAGE_COUNT> stage_time_us{};
	};

	struct CpuStageProfilerSnapshot
	{
		bool valid = false;
		u64 scheduler_entries = 0;
		u64 ee_full_scheduler_entries = 0;
		u64 ee_iop_only_scheduler_entries = 0;
		u64 iop_retained_wait_scheduler_entries = 0;
		u64 stage_samples = 0;
		u64 unbalanced_samples = 0;
		u64 interval_records = 0;
		u64 overwritten_interval_records = 0;
		u64 iop_deadline_gate_checks = 0;
		u64 iop_deadline_gate_skips = 0;
		u64 iop_deadline_gate_dispatches = 0;
		u64 iop_deadline_shadow_late = 0;
		u64 iop_deadline_due = 0;
		u64 iop_counter_due = 0;
		u64 iop_intc_visible = 0;
		u64 iop_callback_due = 0;
		u64 iop_manufactured_only = 0;
		u64 iop_counter_full_updates = 0;
		u64 iop_counter_spu2_only_updates = 0;
		u64 iop_spu2_unconstrained_updates = 0;
		u64 iop_spu2_irq_limited_updates = 0;
		u64 iop_spu2_dma_limited_updates = 0;
		u64 iop_spu2_auto_dma_active_updates = 0;
		u64 ee_deadline_shadow_entries = 0;
		u64 ee_deadline_owner_iop = 0;
		u64 ee_deadline_owner_counter = 0;
		u64 ee_deadline_owner_event = 0;
		u64 ee_deadline_owner_none = 0;
#if defined(VITASX2_CPU_PROFILER)
		std::array<u64, EE_DEADLINE_EVENT_SLOT_COUNT>
			ee_deadline_event_owners{};
#endif
		u64 ee_deadline_horizon_le_3072 = 0;
		u64 ee_deadline_horizon_gt_3072 = 0;
		u64 ee_deadline_horizon_gt_6144 = 0;
		u64 ee_deadline_horizon_gt_12288 = 0;
		u64 ee_deadline_timer_enabled = 0;
		u64 ee_deadline_timer_within_3072 = 0;
		u64 ee_deadline_owner_beyond_3072_timer_off = 0;
		u64 ee_deadline_iop_rapid = 0;
		u64 ee_iop_balance_positive = 0;
		u64 ee_iop_balance_nonpositive = 0;
		u64 ee_iop_ahead_gt_3072 = 0;
		u64 ee_wait_shadow_entries = 0;
		u64 ee_wait_generic_ram = 0;
		u64 ee_wait_poll_call_ram = 0;
		u64 ee_wait_two_predicate_ram = 0;
		u64 ee_wait_retained_unconditional = 0;
		u64 ee_wait_gs_csr_vsint = 0;
		u64 ee_wait_dmac_chcr_str = 0;
		u64 ee_wait_intc_vblank_start_and_ram = 0;
		u64 joint_wait_shadow_entries = 0;
		u64 joint_wait_unknown_writer = 0;
		u64 joint_wait_blocked = 0;
		u64 joint_wait_qualified = 0;
		u64 joint_wait_horizon_gt_6144 = 0;
		u64 joint_wait_horizon_gt_12288 = 0;
		u64 joint_wait_horizon_gt_24576 = 0;
		u64 joint_wait_ram_certified = 0;
		u64 joint_wait_ram_write_overlaps = 0;
		u64 joint_wait_ram_write_overlaps_outside_scheduler = 0;
		u64 joint_wait_activations = 0;
		u64 joint_wait_scheduled_ee_cycles = 0;
		u64 silent_hsync_fold_attempts = 0;
		u64 silent_hsync_fold_activations = 0;
		u64 silent_hsync_folded_edges = 0;
		u64 silent_hsync_fold_blocked_control = 0;
		u64 silent_hsync_fold_blocked_horizon_due = 0;
		u64 silent_hsync_fold_blocked_ee_counter = 0;
		u64 silent_hsync_fold_blocked_iop_counter = 0;
		u64 silent_hsync_fold_blocked_hsync_due = 0;
		u64 silent_hsync_fold_blocked_limit = 0;
		u64 silent_hsync_fold_blocked_hsint = 0;
		u32 silent_hsync_fold_last_stop_detail = 0;
		u32 joint_wait_last_ram_offset = UINT32_MAX;
		u32 joint_wait_last_ram_size = 0;
#if defined(VITASX2_CPU_PROFILER)
		u64 ipu_epoch_from_ipu_wait_entries = 0;
		u64 ipu_epoch_candidate_entries = 0;
		u64 ipu_epoch_candidate_chains = 0;
		u64 ipu_epoch_candidate_continuations = 0;
		u64 ipu_epoch_due_from_ipu = 0;
		u64 ipu_epoch_due_to_ipu = 0;
		u64 ipu_epoch_due_process = 0;
		u64 ipu_epoch_blocked_iop_active = 0;
		u64 ipu_epoch_blocked_no_due_ipu = 0;
		u64 ipu_epoch_blocked_due_non_ipu = 0;
		u64 ipu_epoch_blocked_ee_counter = 0;
		u64 ipu_epoch_blocked_cp0_timer = 0;
		u64 ipu_epoch_blocked_visible_exception = 0;
		u64 ipu_epoch_blocked_vu = 0;
		u64 ipu_epoch_blocked_dmac_suspended = 0;
		u64 ipu_epoch_blocked_instant_dma = 0;
		u64 ipu_epoch_chain_length_1 = 0;
		u64 ipu_epoch_chain_length_2_3 = 0;
		u64 ipu_epoch_chain_length_4_7 = 0;
		u64 ipu_epoch_chain_length_8_15 = 0;
		u64 ipu_epoch_chain_length_16_31 = 0;
		u64 ipu_epoch_chain_length_32_63 = 0;
		u64 ipu_epoch_chain_length_64_plus = 0;
		u32 ipu_epoch_longest_chain = 0;
#endif
		u64 spu2_time_update_calls = 0;
		u64 spu2_time_update_samples = 0;
		u64 spu2_time_update_zero_samples = 0;
		u64 spu2_time_update_one_sample = 0;
		u64 spu2_time_update_2_to_15_samples = 0;
		u64 spu2_time_update_16_to_63_samples = 0;
		u64 spu2_time_update_64_plus_samples = 0;
		u64 spu2_sync_periodic = 0;
		u64 spu2_sync_register_reads = 0;
		u64 spu2_sync_register_writes = 0;
		u64 spu2_sync_dma = 0;
		u64 spu2_sync_observers = 0;
		u64 spu2_mixer_probes = 0;
		u64 spu2_mixer_active_voices = 0;
		u64 spu2_mixer_stopped_voices = 0;
		u64 spu2_mixer_sliding_voices = 0;
		u64 spu2_mixer_noise_voices = 0;
		u64 spu2_mixer_modulated_voices = 0;
		u64 spu2_mixer_fx_enabled_cores = 0;
		u64 spu2_mixer_irq_enabled_cores = 0;
		u64 spu2_mixer_reverb_range_cores = 0;
		u64 spu2_mixer_auto_dma_cores = 0;
		u64 spu2_mixer_equivalent_stopped_cores = 0;
#if defined(VITASX2_CPU_PROFILER)
		u64 spu2_mixer_silent_reverb_samples = 0;
		u64 spu2_mixer_silent_reverb_input_rejects = 0;
		u64 spu2_mixer_silent_reverb_irq_rejects = 0;
		u64 spu2_mixer_silent_reverb_range_rejects = 0;
		u64 spu2_mixer_silent_reverb_state_rejects = 0;
		u64 spu2_stopped_voice_batch_calls = 0;
		u64 spu2_stopped_voice_batch_samples = 0;
		u64 spu2_stopped_voice_bulk_voice_samples = 0;
		u64 spu2_zero_input_reverb_batch_calls = 0;
		u64 spu2_zero_input_reverb_batch_core_samples = 0;
#endif
		// Unlike the sparse stage sampler, these diagnostic-only totals observe
		// every cold EE compilation. This prevents a compiler burst from
		// aliasing against the fixed scheduler sample cadence.
		u64 ee_compile_observations = 0;
		u64 ee_compile_time_us = 0;
#if defined(VITASX2_CPU_PROFILER)
		u64 iop_compile_observations = 0;
		u64 iop_compile_time_us = 0;
		// The statistical sampler runs on a non-EE Vita thread. CPU0 publishes
		// only its current stage with a relaxed word store; the observer samples
		// that marker at a decorrelated cadence. This attributes short helpers
		// without putting two process-clock syscalls around each guest operation.
		u64 statistical_samples = 0;
		u64 statistical_invalid_samples = 0;
		u64 statistical_sampler_cpu_us = 0;
		u64 statistical_ee_pc_sequence = 0;
		u64 statistical_iop_pc_sequence = 0;
		std::array<u64, CPU_STAGE_COUNT> statistical_stage_samples{};
#endif
		std::array<u64, CPU_STAGE_COUNT> stage_time_us{};
		std::array<u64, CPU_STAGE_COUNT> stage_entries{};
	};

	struct CpuProfileHotEdge
	{
		u32 start_pc = 0;
		u32 end_pc = 0;
		u32 samples = 0;
		u64 host_time_us = 0;
		u64 guest_cycles = 0;
		std::array<u32, CPU_PROFILE_CODE_WORD_COUNT> code_start{};
	};

	struct CpuProfileHotEdgeSnapshot
	{
		bool valid = false;
		u64 first_sequence = 0;
		u64 next_sequence = 0;
		u64 dropped_records = 0;
		u64 invalid_records = 0;
		std::array<CpuProfileHotEdge, CPU_PROFILE_HOT_EDGE_COUNT> ee{};
		std::array<CpuProfileHotEdge, CPU_PROFILE_HOT_EDGE_COUNT> iop{};
	};

	struct CpuProfileHotPc
	{
		u32 pc = 0;
		u32 samples = 0;
#if defined(VITASX2_CPU_PROFILER)
		u32 code_words = 0;
		std::array<u32, CPU_PROFILE_CODE_WORD_COUNT> code{};
#endif
	};

	struct CpuProfileHotPcSnapshot
	{
		bool valid = false;
		u64 first_sequence = 0;
		u64 next_sequence = 0;
		u64 dropped_samples = 0;
		u64 invalid_samples = 0;
		std::array<CpuProfileHotPc,
			CPU_PROFILE_HOT_IOP_PC_COUNT> pcs{};
	};

	// SCE_SYSMODULE_PERF is devkit-only and is rejected on retail hardware.
	// The CEX profiler therefore uses the documented process-time clock on a
	// sparse subset of EE scheduler entries. It is diagnostic-only and excluded
	// from normal builds.
	void ConfigureCpuStageProfilerBeforeVmStart(bool enabled);
#if defined(VITASX2_CPU_PROFILER)
	void ShutdownCpuStageProfiler();
#else
	inline void ShutdownCpuStageProfiler()
	{
	}
#endif
	CpuStageProfilerSnapshot GetCpuStageProfilerSnapshot();
	CpuProfileHotEdgeSnapshot GetCpuProfileHotEdgeSnapshot(
		u64 first_sequence, u64 next_sequence);
#if defined(VITASX2_CPU_PROFILER)
	void RegisterIopGeneratedBlockCode(
		u32 pc, const u32* code, u32 code_words);
	CpuProfileHotPcSnapshot GetCpuProfileHotEePcSnapshot(
		u64 first_sequence, u64 next_sequence);
	CpuProfileHotPcSnapshot GetCpuProfileHotIopPcSnapshot(
		u64 first_sequence, u64 next_sequence);
#else
	inline void RegisterIopGeneratedBlockCode(
		u32 pc, const u32* code, u32 code_words)
	{
		(void)pc;
		(void)code;
		(void)code_words;
	}
	inline CpuProfileHotPcSnapshot GetCpuProfileHotEePcSnapshot(
		u64 first_sequence, u64 next_sequence)
	{
		(void)first_sequence;
		(void)next_sequence;
		return {};
	}
	inline CpuProfileHotPcSnapshot GetCpuProfileHotIopPcSnapshot(
		u64 first_sequence, u64 next_sequence)
	{
		(void)first_sequence;
		(void)next_sequence;
		return {};
	}
#endif

	// These are deliberately plain CPU-thread-owned values. Configuration is
	// immutable before execution starts, and no other thread enters the EE
	// scheduler. Normal builds compile every scheduler marker away.
#if defined(VITASX2_CPU_PROFILER)
	extern bool g_cpu_stage_profiler_enabled;
	extern bool g_cpu_stage_sample_active;
	extern std::atomic<u32> g_cpu_stage_statistical_marker;
	extern std::atomic<u32> g_cpu_ee_statistical_pc;
	extern std::atomic<u32> g_cpu_iop_statistical_pc;
#endif

	void OnEeSchedulerEntryEnabled(u32 ee_pc, u64 ee_cycle,
		u32 iop_pc, u64 iop_cycle);
	void OnEeSchedulerExitEnabled();
	void BeginCpuStage(CpuStage stage);
	void EndCpuStage();
	u32 BeginExactEeCompileMeasurement();
	void EndExactEeCompileMeasurement(u32 start_us);
	u32 BeginExactIopCompileMeasurement();
	void EndExactIopCompileMeasurement(u32 start_us);
	void CountCpuStageEntry(CpuStage stage);
	void RecordIopDeadlineGate(bool dispatched, bool deadline_due,
		bool counter_due, bool counter_precedes_published, bool intc_visible,
		bool callback_due);
	void RecordIopCounterUpdate(
		bool spu2_only, u32 spu2_deadline_constraints);
	void RecordEeDeadlineHorizon(s64 owner_horizon_delta,
		EeDeadlineOwner owner, u32 ee_event_owner, s32 ee_iop_balance,
		bool timer_enabled, u32 timer_delta, bool iop_rapid);
	void RecordEeSchedulerPath(bool iop_only, bool iop_retained_wait);
	void RecordJointWaitShadow(u32 ee_wait_origin, bool iop_retained_wait,
		s64 horizon_delta, bool unknown_writer, bool blocked,
		u32 ram_offset, u32 ram_size);
	void RecordJointWaitRamWriteOverlap(bool scheduler_active);
	void RecordJointWaitActivation(u32 scheduled_ee_cycles);
	void RecordSilentHsyncFold(
		u32 folded_edges, u32 stop_reason, u32 stop_detail);
#if defined(VITASX2_CPU_PROFILER)
	void RecordIpuEpochOpportunity(bool from_ipu_wait, u32 wait_pc,
		u32 due_ipu_mask, u32 blocker_mask);
#endif
	void RecordSpu2TimeUpdate(u32 samples);
	void RecordSpu2SyncReason(Spu2SyncReason reason);
	void RecordSpu2MixerProbe(u32 active_voices, u32 stopped_voices,
		u32 sliding_voices, u32 noise_voices, u32 modulated_voices,
		u32 fx_enabled_cores, u32 irq_enabled_cores,
		u32 reverb_range_cores, u32 auto_dma_cores,
		u32 equivalent_stopped_cores, u32 silent_reverb_samples,
		u32 silent_reverb_input_rejects,
		u32 silent_reverb_irq_rejects,
		u32 silent_reverb_range_rejects,
		u32 silent_reverb_state_rejects);
#if defined(VITASX2_CPU_PROFILER)
	void RecordSpu2StoppedVoiceBatch(
		u32 samples, u32 stable_core0_mask, u32 stable_core1_mask,
		u32 zero_input_reverb_core_mask);
#endif

	inline void OnEeSchedulerEntry(u32 ee_pc, u64 ee_cycle,
		u32 iop_pc, u64 iop_cycle)
	{
#if defined(VITASX2_CPU_PROFILER)
		if (g_cpu_stage_profiler_enabled)
			OnEeSchedulerEntryEnabled(ee_pc, ee_cycle, iop_pc, iop_cycle);
#else
		(void)ee_pc;
		(void)ee_cycle;
		(void)iop_pc;
		(void)iop_cycle;
#endif
	}

	inline void OnEeSchedulerExit()
	{
#if defined(VITASX2_CPU_PROFILER)
		if (g_cpu_stage_profiler_enabled)
			OnEeSchedulerExitEnabled();
#endif
	}

	inline void BeginCpuStageIfSampling(CpuStage stage)
	{
#if defined(VITASX2_CPU_PROFILER)
		if (g_cpu_stage_sample_active)
			BeginCpuStage(stage);
#else
		(void)stage;
#endif
	}

	inline void EndCpuStageIfSampling()
	{
#if defined(VITASX2_CPU_PROFILER)
		if (g_cpu_stage_sample_active)
			EndCpuStage();
#endif
	}

	inline void CountCpuStageEntryIfSampling(CpuStage stage)
	{
#if defined(VITASX2_CPU_PROFILER)
		if (g_cpu_stage_sample_active)
			CountCpuStageEntry(stage);
#else
		(void)stage;
#endif
	}

	inline void PublishIopGeneratedPcIfProfiling(u32 pc)
	{
#if defined(VITASX2_CPU_PROFILER)
		if (g_cpu_stage_profiler_enabled)
			g_cpu_iop_statistical_pc.store(pc, std::memory_order_relaxed);
#else
		(void)pc;
#endif
	}

#if defined(VITASX2_CPU_PROFILER)
	inline void PublishCpuStatisticalStageIfProfiling(CpuStage stage)
	{
		if (g_cpu_stage_profiler_enabled)
		{
			g_cpu_stage_statistical_marker.store(
				static_cast<u32>(stage), std::memory_order_relaxed);
		}
	}
#endif

	inline void RecordIopDeadlineGateIfProfiling(
		bool dispatched, bool deadline_due, bool counter_due,
		bool counter_precedes_published, bool intc_visible, bool callback_due)
	{
#if defined(VITASX2_CPU_PROFILER)
		if (g_cpu_stage_profiler_enabled)
		{
			RecordIopDeadlineGate(dispatched, deadline_due, counter_due,
				counter_precedes_published, intc_visible, callback_due);
		}
#else
		(void)dispatched;
		(void)deadline_due;
		(void)counter_due;
		(void)counter_precedes_published;
		(void)intc_visible;
		(void)callback_due;
#endif
	}

	inline void RecordEeDeadlineHorizonIfProfiling(
		s64 owner_horizon_delta, EeDeadlineOwner owner, u32 ee_event_owner,
		s32 ee_iop_balance, bool timer_enabled, u32 timer_delta,
		bool iop_rapid)
	{
#if defined(VITASX2_CPU_PROFILER)
		if (g_cpu_stage_profiler_enabled)
		{
			RecordEeDeadlineHorizon(owner_horizon_delta, owner,
				ee_event_owner, ee_iop_balance, timer_enabled,
				timer_delta, iop_rapid);
		}
#else
		(void)owner_horizon_delta;
		(void)owner;
		(void)ee_event_owner;
		(void)ee_iop_balance;
		(void)timer_enabled;
		(void)timer_delta;
		(void)iop_rapid;
#endif
	}

	inline void RecordIopCounterUpdateIfProfiling(bool spu2_only,
		u32 spu2_deadline_constraints)
	{
#if defined(VITASX2_CPU_PROFILER)
		if (g_cpu_stage_profiler_enabled)
			RecordIopCounterUpdate(spu2_only, spu2_deadline_constraints);
#else
		(void)spu2_only;
		(void)spu2_deadline_constraints;
#endif
	}

	inline void RecordEeSchedulerPathIfProfiling(bool iop_only,
		bool iop_retained_wait)
	{
#if defined(VITASX2_CPU_PROFILER)
		if (g_cpu_stage_profiler_enabled)
			RecordEeSchedulerPath(iop_only, iop_retained_wait);
#else
		(void)iop_only;
		(void)iop_retained_wait;
#endif
	}

	inline void RecordJointWaitShadowIfProfiling(u32 ee_wait_origin,
		bool iop_retained_wait, s64 horizon_delta, bool unknown_writer,
		bool blocked, u32 ram_offset, u32 ram_size)
	{
#if defined(VITASX2_CPU_PROFILER)
		if (g_cpu_stage_profiler_enabled)
		{
			RecordJointWaitShadow(ee_wait_origin, iop_retained_wait,
				horizon_delta, unknown_writer, blocked,
				ram_offset, ram_size);
		}
#else
		(void)ee_wait_origin;
		(void)iop_retained_wait;
		(void)horizon_delta;
		(void)unknown_writer;
		(void)blocked;
		(void)ram_offset;
		(void)ram_size;
#endif
	}

	inline void RecordJointWaitRamWriteOverlapIfProfiling(
		bool scheduler_active)
	{
#if defined(VITASX2_CPU_PROFILER)
		if (g_cpu_stage_profiler_enabled)
			RecordJointWaitRamWriteOverlap(scheduler_active);
#else
		(void)scheduler_active;
#endif
	}

	inline void RecordJointWaitActivationIfProfiling(
		u32 scheduled_ee_cycles)
	{
#if defined(VITASX2_CPU_PROFILER)
		if (g_cpu_stage_profiler_enabled)
			RecordJointWaitActivation(scheduled_ee_cycles);
#else
		(void)scheduled_ee_cycles;
#endif
	}

	inline void RecordSilentHsyncFoldIfProfiling(
		u32 folded_edges, u32 stop_reason, u32 stop_detail)
	{
#if defined(VITASX2_CPU_PROFILER)
		if (g_cpu_stage_profiler_enabled)
			RecordSilentHsyncFold(
				folded_edges, stop_reason, stop_detail);
#else
		(void)folded_edges;
		(void)stop_reason;
		(void)stop_detail;
#endif
	}

#if defined(VITASX2_CPU_PROFILER)
	inline void RecordIpuEpochOpportunityIfProfiling(bool from_ipu_wait,
		u32 wait_pc, u32 due_ipu_mask, u32 blocker_mask)
	{
		if (g_cpu_stage_profiler_enabled)
		{
			RecordIpuEpochOpportunity(from_ipu_wait, wait_pc,
				due_ipu_mask, blocker_mask);
		}
	}

#endif

	inline void RecordSpu2TimeUpdateIfProfiling(u32 samples)
	{
#if defined(VITASX2_CPU_PROFILER)
		if (g_cpu_stage_profiler_enabled)
			RecordSpu2TimeUpdate(samples);
#else
		(void)samples;
#endif
	}

	inline void RecordSpu2SyncReasonIfProfiling(Spu2SyncReason reason)
	{
#if defined(VITASX2_CPU_PROFILER)
		if (g_cpu_stage_profiler_enabled)
			RecordSpu2SyncReason(reason);
#else
		(void)reason;
#endif
	}

	inline void RecordSpu2MixerProbeIfProfiling(
		u32 active_voices, u32 stopped_voices, u32 sliding_voices,
		u32 noise_voices, u32 modulated_voices, u32 fx_enabled_cores,
		u32 irq_enabled_cores, u32 reverb_range_cores,
		u32 auto_dma_cores, u32 equivalent_stopped_cores,
		u32 silent_reverb_samples, u32 silent_reverb_input_rejects,
		u32 silent_reverb_irq_rejects,
		u32 silent_reverb_range_rejects,
		u32 silent_reverb_state_rejects)
	{
#if defined(VITASX2_CPU_PROFILER)
		if (g_cpu_stage_profiler_enabled)
		{
			RecordSpu2MixerProbe(active_voices, stopped_voices,
				sliding_voices, noise_voices, modulated_voices,
				fx_enabled_cores, irq_enabled_cores,
				reverb_range_cores, auto_dma_cores,
				equivalent_stopped_cores, silent_reverb_samples,
				silent_reverb_input_rejects,
				silent_reverb_irq_rejects,
				silent_reverb_range_rejects,
				silent_reverb_state_rejects);
		}
#else
		(void)active_voices;
		(void)stopped_voices;
		(void)sliding_voices;
		(void)noise_voices;
		(void)modulated_voices;
		(void)fx_enabled_cores;
		(void)irq_enabled_cores;
		(void)reverb_range_cores;
		(void)auto_dma_cores;
		(void)equivalent_stopped_cores;
		(void)silent_reverb_samples;
		(void)silent_reverb_input_rejects;
		(void)silent_reverb_irq_rejects;
		(void)silent_reverb_range_rejects;
		(void)silent_reverb_state_rejects;
#endif
	}

	inline void RecordSpu2StoppedVoiceBatchIfProfiling(
		u32 samples, u32 stable_core0_mask, u32 stable_core1_mask,
		u32 zero_input_reverb_core_mask)
	{
#if defined(VITASX2_CPU_PROFILER)
		if (g_cpu_stage_profiler_enabled)
		{
			RecordSpu2StoppedVoiceBatch(
				samples, stable_core0_mask, stable_core1_mask,
				zero_input_reverb_core_mask);
		}
#else
		(void)samples;
		(void)stable_core0_mask;
		(void)stable_core1_mask;
		(void)zero_input_reverb_core_mask;
#endif
	}

	class ScopedCpuStage
	{
	public:
		explicit ScopedCpuStage(CpuStage stage)
		{
#if defined(VITASX2_CPU_PROFILER)
			m_entered = g_cpu_stage_profiler_enabled;
			if (m_entered)
			{
				m_previous_statistical_stage =
					g_cpu_stage_statistical_marker.load(
						std::memory_order_relaxed);
				g_cpu_stage_statistical_marker.store(
					static_cast<u32>(stage),
					std::memory_order_relaxed);
				m_sampled = g_cpu_stage_sample_active;
				if (m_sampled)
					BeginCpuStage(stage);
			}
#else
			(void)stage;
#endif
		}

		~ScopedCpuStage()
		{
#if defined(VITASX2_CPU_PROFILER)
			if (m_sampled && g_cpu_stage_sample_active)
				EndCpuStage();
			if (m_entered)
			{
				g_cpu_stage_statistical_marker.store(
					m_previous_statistical_stage,
					std::memory_order_relaxed);
			}
#endif
		}

		ScopedCpuStage(const ScopedCpuStage&) = delete;
		ScopedCpuStage& operator=(const ScopedCpuStage&) = delete;

	private:
#if defined(VITASX2_CPU_PROFILER)
		bool m_entered = false;
		bool m_sampled = false;
		u32 m_previous_statistical_stage =
			static_cast<u32>(CpuStage::Count);
#endif
	};

	class ScopedExactEeCompileMeasurement
	{
	public:
		ScopedExactEeCompileMeasurement()
		{
#if defined(VITASX2_CPU_PROFILER)
			m_enabled = g_cpu_stage_profiler_enabled;
			if (m_enabled)
				m_start_us = BeginExactEeCompileMeasurement();
#endif
		}

		~ScopedExactEeCompileMeasurement()
		{
#if defined(VITASX2_CPU_PROFILER)
			if (m_enabled)
				EndExactEeCompileMeasurement(m_start_us);
#endif
		}

		ScopedExactEeCompileMeasurement(
			const ScopedExactEeCompileMeasurement&) = delete;
		ScopedExactEeCompileMeasurement& operator=(
			const ScopedExactEeCompileMeasurement&) = delete;

	private:
#if defined(VITASX2_CPU_PROFILER)
		u32 m_start_us = 0;
		bool m_enabled = false;
#endif
	};

	class ScopedExactIopCompileMeasurement
	{
	public:
		ScopedExactIopCompileMeasurement()
		{
#if defined(VITASX2_CPU_PROFILER)
			m_enabled = g_cpu_stage_profiler_enabled;
			if (m_enabled)
				m_start_us = BeginExactIopCompileMeasurement();
#endif
		}

		~ScopedExactIopCompileMeasurement()
		{
#if defined(VITASX2_CPU_PROFILER)
			if (m_enabled)
				EndExactIopCompileMeasurement(m_start_us);
#endif
		}

		ScopedExactIopCompileMeasurement(
			const ScopedExactIopCompileMeasurement&) = delete;
		ScopedExactIopCompileMeasurement& operator=(
			const ScopedExactIopCompileMeasurement&) = delete;

	private:
#if defined(VITASX2_CPU_PROFILER)
		u32 m_start_us = 0;
		bool m_enabled = false;
#endif
	};
}
