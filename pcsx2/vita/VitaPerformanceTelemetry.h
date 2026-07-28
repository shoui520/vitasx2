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
		Count,
	};

	static constexpr size_t CPU_STAGE_COUNT =
		static_cast<size_t>(CpuStage::Count);
	static constexpr u32 CPU_STAGE_SAMPLE_PERIOD = 1024;

	struct CpuStageProfilerSnapshot
	{
		bool valid = false;
		u64 scheduler_entries = 0;
		u64 stage_samples = 0;
		u64 unbalanced_samples = 0;
		std::array<u64, CPU_STAGE_COUNT> stage_time_us{};
		std::array<u64, CPU_STAGE_COUNT> stage_entries{};
	};

	// SCE_SYSMODULE_PERF is devkit-only and is rejected on retail hardware.
	// The CEX profiler therefore uses the documented process-time clock on a
	// sparse subset of EE scheduler entries. It is diagnostic-only and excluded
	// from normal builds.
	void ConfigureCpuStageProfilerBeforeVmStart(bool enabled);
	CpuStageProfilerSnapshot GetCpuStageProfilerSnapshot();

	// These are deliberately plain CPU-thread-owned values. Configuration is
	// immutable before execution starts, and no other thread enters the EE
	// scheduler. Normal builds compile every scheduler marker away.
#if defined(VITASX2_CPU_PROFILER)
	extern bool g_cpu_stage_profiler_enabled;
	extern bool g_cpu_stage_sample_active;
#endif

	void OnEeSchedulerEntryEnabled();
	void OnEeSchedulerExitEnabled();
	void BeginCpuStage(CpuStage stage);
	void EndCpuStage();

	inline void OnEeSchedulerEntry()
	{
#if defined(VITASX2_CPU_PROFILER)
		if (g_cpu_stage_profiler_enabled)
			OnEeSchedulerEntryEnabled();
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
}
