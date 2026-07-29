// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <array>
#include <cstddef>

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
		EeProvider,
		EeInterpreter,
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
	};

	static constexpr size_t CPU_STAGE_COUNT =
		static_cast<size_t>(CpuStage::Count);
	static constexpr u32 CPU_STAGE_SAMPLE_PERIOD = 1024;
	// PES currently produces roughly 390 samples per 120-VSync measurement
	// window at the 1/1024 cadence.  512 records retain a complete ordinary
	// window while remaining below 96 KiB.
	static constexpr size_t CPU_PROFILE_INTERVAL_RING_SIZE = 512;
	static constexpr size_t CPU_PROFILE_HOT_EDGE_COUNT = 8;

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
		u16 flags = 0;
		u16 stage_count = 0;
		std::array<u32, CPU_STAGE_COUNT> stage_time_us{};
	};

	struct CpuStageProfilerSnapshot
	{
		bool valid = false;
		u64 scheduler_entries = 0;
		u64 stage_samples = 0;
		u64 unbalanced_samples = 0;
		u64 interval_records = 0;
		u64 overwritten_interval_records = 0;
		u64 iop_deadline_gate_checks = 0;
		u64 iop_deadline_gate_skips = 0;
		u64 iop_deadline_gate_dispatches = 0;
		u64 iop_deadline_shadow_late = 0;
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

	// SCE_SYSMODULE_PERF is devkit-only and is rejected on retail hardware.
	// The CEX profiler therefore uses the documented process-time clock on a
	// sparse subset of EE scheduler entries. It is diagnostic-only and excluded
	// from normal builds.
	void ConfigureCpuStageProfilerBeforeVmStart(bool enabled);
	CpuStageProfilerSnapshot GetCpuStageProfilerSnapshot();
	CpuProfileHotEdgeSnapshot GetCpuProfileHotEdgeSnapshot(
		u64 first_sequence, u64 next_sequence);

	// These are deliberately plain CPU-thread-owned values. Configuration is
	// immutable before execution starts, and no other thread enters the EE
	// scheduler. Normal builds compile every scheduler marker away.
#if defined(VITASX2_CPU_PROFILER)
	extern bool g_cpu_stage_profiler_enabled;
	extern bool g_cpu_stage_sample_active;
#endif

	void OnEeSchedulerEntryEnabled(u32 ee_pc, u64 ee_cycle,
		u32 iop_pc, u64 iop_cycle);
	void OnEeSchedulerExitEnabled();
	void BeginCpuStage(CpuStage stage);
	void EndCpuStage();
	void CountCpuStageEntry(CpuStage stage);
	void RecordIopDeadlineGate(bool dispatched, bool shadow_late);

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

	inline void RecordIopDeadlineGateIfProfiling(
		bool dispatched, bool shadow_late)
	{
#if defined(VITASX2_CPU_PROFILER)
		if (g_cpu_stage_profiler_enabled)
			RecordIopDeadlineGate(dispatched, shadow_late);
#else
		(void)dispatched;
		(void)shadow_late;
#endif
	}

	class ScopedCpuStage
	{
	public:
		explicit ScopedCpuStage(CpuStage stage)
		{
#if defined(VITASX2_CPU_PROFILER)
			m_entered = g_cpu_stage_sample_active;
			if (m_entered)
				BeginCpuStage(stage);
#else
			(void)stage;
#endif
		}

		~ScopedCpuStage()
		{
#if defined(VITASX2_CPU_PROFILER)
			if (m_entered && g_cpu_stage_sample_active)
				EndCpuStage();
#endif
		}

		ScopedCpuStage(const ScopedCpuStage&) = delete;
		ScopedCpuStage& operator=(const ScopedCpuStage&) = delete;

	private:
#if defined(VITASX2_CPU_PROFILER)
		bool m_entered = false;
#endif
	};
}
