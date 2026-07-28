// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaPerformanceTelemetry.h"

#if defined(__vita__) && defined(VITASX2_CPU_PROFILER)
#include <psp2/kernel/processmgr.h>
#endif

#include <array>

namespace VitaPerformanceTelemetry
{
	bool g_enabled = true;
	bool g_cpu_stage_profiler_enabled = false;
	bool g_cpu_stage_sample_active = false;

	namespace
	{
		constexpr u32 STAGE_STACK_DEPTH = 8;

		struct CpuStageProfilerState
		{
			u32 scheduler_until_sample = CPU_STAGE_SAMPLE_PERIOD;
			CpuStageProfilerSnapshot totals;
			CpuStage current_stage = CpuStage::Scheduler;
			std::array<CpuStage, STAGE_STACK_DEPTH> stage_stack{};
			u32 stage_depth = 0;
			u32 stage_start_us = 0;
		};
		CpuStageProfilerState s_cpu_stage_profiler;

		inline u32 ReadProcessTimeLow()
		{
#if defined(__vita__) && defined(VITASX2_CPU_PROFILER)
			return sceKernelGetProcessTimeLow();
#else
			return 0;
#endif
		}

		void AccumulateCurrentStage(u32 now)
		{
			const size_t stage =
				static_cast<size_t>(s_cpu_stage_profiler.current_stage);
			s_cpu_stage_profiler.totals.stage_time_us[stage] +=
				static_cast<u32>(now -
					s_cpu_stage_profiler.stage_start_us);
			s_cpu_stage_profiler.stage_start_us = now;
		}
	} // namespace

	void SetEnabledBeforeVmStart(bool enabled)
	{
		g_enabled = enabled;
	}

	void ConfigureCpuStageProfilerBeforeVmStart(bool enabled)
	{
		g_cpu_stage_profiler_enabled = enabled;
		g_cpu_stage_sample_active = false;
		s_cpu_stage_profiler = {};
		s_cpu_stage_profiler.scheduler_until_sample =
			CPU_STAGE_SAMPLE_PERIOD;
		s_cpu_stage_profiler.totals.valid = enabled;
	}

	CpuStageProfilerSnapshot GetCpuStageProfilerSnapshot()
	{
		return s_cpu_stage_profiler.totals;
	}

	void OnEeSchedulerEntryEnabled()
	{
		s_cpu_stage_profiler.totals.scheduler_entries++;
		if (--s_cpu_stage_profiler.scheduler_until_sample != 0)
			return;

		s_cpu_stage_profiler.scheduler_until_sample =
			CPU_STAGE_SAMPLE_PERIOD;
		g_cpu_stage_sample_active = true;
		s_cpu_stage_profiler.current_stage = CpuStage::Scheduler;
		s_cpu_stage_profiler.stage_depth = 0;
		s_cpu_stage_profiler.stage_start_us = ReadProcessTimeLow();
		s_cpu_stage_profiler.totals.stage_entries[
			static_cast<size_t>(CpuStage::Scheduler)]++;
	}

	void OnEeSchedulerExitEnabled()
	{
		if (!g_cpu_stage_sample_active)
			return;

		AccumulateCurrentStage(ReadProcessTimeLow());
		if (s_cpu_stage_profiler.stage_depth != 0 ||
			s_cpu_stage_profiler.current_stage != CpuStage::Scheduler)
		{
			s_cpu_stage_profiler.totals.unbalanced_samples++;
		}
		s_cpu_stage_profiler.totals.stage_samples++;
		s_cpu_stage_profiler.stage_depth = 0;
		g_cpu_stage_sample_active = false;
	}

	void BeginCpuStage(CpuStage stage)
	{
		if (!g_cpu_stage_sample_active)
			return;

		const u32 now = ReadProcessTimeLow();
		AccumulateCurrentStage(now);
		if (s_cpu_stage_profiler.stage_depth >= STAGE_STACK_DEPTH)
		{
			s_cpu_stage_profiler.totals.unbalanced_samples++;
			g_cpu_stage_sample_active = false;
			return;
		}
		s_cpu_stage_profiler.stage_stack[
			s_cpu_stage_profiler.stage_depth++] =
			s_cpu_stage_profiler.current_stage;
		s_cpu_stage_profiler.current_stage = stage;
		s_cpu_stage_profiler.totals.stage_entries[
			static_cast<size_t>(stage)]++;
	}

	void EndCpuStage()
	{
		if (!g_cpu_stage_sample_active)
			return;

		const u32 now = ReadProcessTimeLow();
		AccumulateCurrentStage(now);
		if (s_cpu_stage_profiler.stage_depth == 0)
		{
			s_cpu_stage_profiler.totals.unbalanced_samples++;
			g_cpu_stage_sample_active = false;
			return;
		}
		s_cpu_stage_profiler.current_stage =
			s_cpu_stage_profiler.stage_stack[
				--s_cpu_stage_profiler.stage_depth];
	}
}
