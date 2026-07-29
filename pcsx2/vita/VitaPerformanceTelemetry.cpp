// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaPerformanceTelemetry.h"

#if defined(__vita__) && defined(VITASX2_CPU_PROFILER)
#include <psp2/kernel/processmgr.h>
#endif

#if defined(VITASX2_CPU_PROFILER)
#include <algorithm>
#include <array>
#endif

namespace VitaPerformanceTelemetry
{
	bool g_enabled = true;
#if defined(VITASX2_CPU_PROFILER)
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
			CpuProfileIntervalRecord current_interval;
			std::array<CpuProfileIntervalRecord,
				CPU_PROFILE_INTERVAL_RING_SIZE> interval_ring{};
			u64 next_interval_sequence = 1;
			size_t interval_write_index = 0;
			size_t interval_record_count = 0;
		};
		static_assert(
			sizeof(CpuProfileIntervalRecord) *
				CPU_PROFILE_INTERVAL_RING_SIZE <= 96 * 1024,
			"CPU0 profile interval ring must remain below 96 KiB");
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
			const u32 elapsed = static_cast<u32>(
				now - s_cpu_stage_profiler.stage_start_us);
			s_cpu_stage_profiler.totals.stage_time_us[stage] += elapsed;
			s_cpu_stage_profiler.current_interval.stage_time_us[stage] += elapsed;
			s_cpu_stage_profiler.stage_start_us = now;
		}

		void FinishCurrentInterval(u32 now, u32 ee_pc, u64 ee_cycle,
			u32 iop_pc, u64 iop_cycle, bool balanced)
		{
			AccumulateCurrentStage(now);
			CpuProfileIntervalRecord& record =
				s_cpu_stage_profiler.current_interval;
			record.process_time_end_us = now;
			record.ee_pc_end = ee_pc;
			record.ee_cycle_end = ee_cycle;
			record.iop_pc_end = iop_pc;
			record.iop_cycle_end = iop_cycle;
			record.flags = balanced ? CpuProfileIntervalBalanced : 0;
			record.stage_count = static_cast<u16>(CPU_STAGE_COUNT);

			s_cpu_stage_profiler.interval_ring[
				s_cpu_stage_profiler.interval_write_index] = record;
			s_cpu_stage_profiler.interval_write_index =
				(s_cpu_stage_profiler.interval_write_index + 1) %
				CPU_PROFILE_INTERVAL_RING_SIZE;
			if (s_cpu_stage_profiler.interval_record_count <
				CPU_PROFILE_INTERVAL_RING_SIZE)
			{
				s_cpu_stage_profiler.interval_record_count++;
			}
			else
			{
				s_cpu_stage_profiler.totals.overwritten_interval_records++;
			}
			s_cpu_stage_profiler.totals.interval_records++;
			s_cpu_stage_profiler.next_interval_sequence++;
			s_cpu_stage_profiler.totals.stage_samples++;
			s_cpu_stage_profiler.stage_depth = 0;
			g_cpu_stage_sample_active = false;
		}
	} // namespace
#endif

	void SetEnabledBeforeVmStart(bool enabled)
	{
		g_enabled = enabled;
	}

	void ConfigureCpuStageProfilerBeforeVmStart(bool enabled)
	{
#if defined(VITASX2_CPU_PROFILER)
		g_cpu_stage_profiler_enabled = enabled;
		g_cpu_stage_sample_active = false;
		s_cpu_stage_profiler = {};
		s_cpu_stage_profiler.scheduler_until_sample =
			CPU_STAGE_SAMPLE_PERIOD;
		s_cpu_stage_profiler.totals.valid = enabled;
#else
		(void)enabled;
#endif
	}

	CpuStageProfilerSnapshot GetCpuStageProfilerSnapshot()
	{
#if defined(VITASX2_CPU_PROFILER)
		return s_cpu_stage_profiler.totals;
#else
		return {};
#endif
	}

	CpuProfileHotEdgeSnapshot GetCpuProfileHotEdgeSnapshot(
		u64 first_sequence, u64 next_sequence)
	{
#if defined(VITASX2_CPU_PROFILER)
		CpuProfileHotEdgeSnapshot snapshot;
		snapshot.valid = s_cpu_stage_profiler.totals.valid;
		snapshot.first_sequence = first_sequence;
		snapshot.next_sequence = next_sequence;
		if (!snapshot.valid || first_sequence >= next_sequence)
			return snapshot;

		const u64 retained_first =
			s_cpu_stage_profiler.next_interval_sequence -
			static_cast<u64>(s_cpu_stage_profiler.interval_record_count);
		if (first_sequence < retained_first)
		{
			snapshot.dropped_records = retained_first - first_sequence;
			first_sequence = retained_first;
		}
		next_sequence = std::min(
			next_sequence, s_cpu_stage_profiler.next_interval_sequence);

		constexpr size_t CANDIDATE_COUNT = 32;
		struct Candidate
		{
			u32 start_pc = 0;
			u32 end_pc = 0;
			u32 estimate = 0;
			bool valid = false;
		};

		const auto record_at_sequence = [](u64 sequence)
			-> const CpuProfileIntervalRecord& {
			const u64 distance =
				s_cpu_stage_profiler.next_interval_sequence - sequence;
			const size_t index =
				(s_cpu_stage_profiler.interval_write_index +
				 CPU_PROFILE_INTERVAL_RING_SIZE -
				 static_cast<size_t>(distance)) %
				CPU_PROFILE_INTERVAL_RING_SIZE;
			return s_cpu_stage_profiler.interval_ring[index];
		};
		const auto select_candidates = [&record_at_sequence, &snapshot,
			first_sequence, next_sequence](bool ee) {
			std::array<Candidate, CANDIDATE_COUNT> candidates{};
			for (u64 sequence = first_sequence;
				sequence < next_sequence; sequence++)
			{
				const CpuProfileIntervalRecord& record =
					record_at_sequence(sequence);
				if (record.sequence != sequence)
				{
					if (ee)
						snapshot.invalid_records++;
					continue;
				}
				if ((record.flags & CpuProfileIntervalBalanced) == 0)
					continue;
				const u32 start_pc = ee ?
					record.ee_pc_start : record.iop_pc_start;
				const u32 end_pc = ee ?
					record.ee_pc_end : record.iop_pc_end;
				Candidate* empty = nullptr;
				Candidate* least = &candidates[0];
				bool matched = false;
				for (Candidate& candidate : candidates)
				{
					if (candidate.valid &&
						candidate.start_pc == start_pc &&
						candidate.end_pc == end_pc)
					{
						candidate.estimate++;
						matched = true;
						break;
					}
					if (!candidate.valid && !empty)
						empty = &candidate;
					if (candidate.estimate < least->estimate)
						least = &candidate;
				}
				if (matched)
					continue;
				Candidate* const target = empty ? empty : least;
				const u32 inherited = target->valid ?
					target->estimate : 0;
				*target = {start_pc, end_pc, inherited + 1, true};
			}
			return candidates;
		};
		const auto exact_edges = [&record_at_sequence, first_sequence,
			next_sequence](bool ee,
				const std::array<Candidate, CANDIDATE_COUNT>& candidates) {
			const auto domain_host_time = [](const CpuProfileIntervalRecord& record,
				bool ee_domain) {
				const auto stage_time = [&record](CpuStage stage) {
					return static_cast<u64>(record.stage_time_us[
						static_cast<size_t>(stage)]);
				};
				if (ee_domain)
				{
					return stage_time(CpuStage::EeGenerated) +
						stage_time(CpuStage::EeProvider) +
						stage_time(CpuStage::EeInterpreter) +
						stage_time(CpuStage::Cop1) +
						stage_time(CpuStage::EeHelper) +
						stage_time(CpuStage::EeMemorySlowPath);
				}
				return stage_time(CpuStage::IopGuest) +
					stage_time(CpuStage::IopInterpreter) +
					stage_time(CpuStage::IopHelper) +
					stage_time(CpuStage::IopMemorySlowPath);
			};
			std::array<CpuProfileHotEdge, CANDIDATE_COUNT> edges{};
			for (size_t i = 0; i < candidates.size(); i++)
			{
				if (!candidates[i].valid)
					continue;
				edges[i].start_pc = candidates[i].start_pc;
				edges[i].end_pc = candidates[i].end_pc;
			}
			for (u64 sequence = first_sequence;
				sequence < next_sequence; sequence++)
			{
				const CpuProfileIntervalRecord& record =
					record_at_sequence(sequence);
				if (record.sequence != sequence)
					continue;
				if ((record.flags & CpuProfileIntervalBalanced) == 0)
					continue;
				const u32 start_pc = ee ?
					record.ee_pc_start : record.iop_pc_start;
				const u32 end_pc = ee ?
					record.ee_pc_end : record.iop_pc_end;
				for (size_t i = 0; i < candidates.size(); i++)
				{
					if (!candidates[i].valid ||
						edges[i].start_pc != start_pc ||
						edges[i].end_pc != end_pc)
					{
						continue;
					}
					CpuProfileHotEdge& edge = edges[i];
					edge.samples++;
					edge.host_time_us += domain_host_time(record, ee);
					const u64 cycle_start = ee ?
						record.ee_cycle_start : record.iop_cycle_start;
					const u64 cycle_end = ee ?
						record.ee_cycle_end : record.iop_cycle_end;
					edge.guest_cycles += cycle_end - cycle_start;
					break;
				}
			}
			std::sort(edges.begin(), edges.end(),
				[](const CpuProfileHotEdge& left,
					const CpuProfileHotEdge& right) {
					if (left.host_time_us != right.host_time_us)
						return left.host_time_us > right.host_time_us;
					return left.samples > right.samples;
				});
			return edges;
		};

		const auto ee_candidates = select_candidates(true);
		const auto iop_candidates = select_candidates(false);
		const auto ee_edges =
			exact_edges(true, ee_candidates);
		const auto iop_edges =
			exact_edges(false, iop_candidates);
		std::copy_n(ee_edges.begin(), snapshot.ee.size(),
			snapshot.ee.begin());
		std::copy_n(iop_edges.begin(), snapshot.iop.size(),
			snapshot.iop.begin());
		return snapshot;
#else
		(void)first_sequence;
		(void)next_sequence;
		return {};
#endif
	}

#if defined(VITASX2_CPU_PROFILER)
	void OnEeSchedulerEntryEnabled(u32 ee_pc, u64 ee_cycle,
		u32 iop_pc, u64 iop_cycle)
	{
		u32 now = 0;
		if (g_cpu_stage_sample_active)
		{
			now = ReadProcessTimeLow();
			const bool balanced =
				s_cpu_stage_profiler.stage_depth == 0 &&
				s_cpu_stage_profiler.current_stage == CpuStage::EeGenerated;
			if (!balanced)
				s_cpu_stage_profiler.totals.unbalanced_samples++;
			FinishCurrentInterval(now, ee_pc, ee_cycle, iop_pc, iop_cycle,
				balanced);
		}

		s_cpu_stage_profiler.totals.scheduler_entries++;
		if (--s_cpu_stage_profiler.scheduler_until_sample != 0)
			return;

		s_cpu_stage_profiler.scheduler_until_sample =
			CPU_STAGE_SAMPLE_PERIOD;
		if (now == 0)
			now = ReadProcessTimeLow();
		g_cpu_stage_sample_active = true;
		s_cpu_stage_profiler.current_stage = CpuStage::Scheduler;
		s_cpu_stage_profiler.stage_depth = 0;
		s_cpu_stage_profiler.stage_start_us = now;
		s_cpu_stage_profiler.current_interval = {};
		s_cpu_stage_profiler.current_interval.sequence =
			s_cpu_stage_profiler.next_interval_sequence;
		s_cpu_stage_profiler.current_interval.process_time_start_us = now;
		s_cpu_stage_profiler.current_interval.ee_pc_start = ee_pc;
		s_cpu_stage_profiler.current_interval.ee_cycle_start = ee_cycle;
		s_cpu_stage_profiler.current_interval.iop_pc_start = iop_pc;
		s_cpu_stage_profiler.current_interval.iop_cycle_start = iop_cycle;
		s_cpu_stage_profiler.totals.stage_entries[
			static_cast<size_t>(CpuStage::Scheduler)]++;
	}

	void OnEeSchedulerExitEnabled()
	{
		if (!g_cpu_stage_sample_active)
			return;

		const u32 now = ReadProcessTimeLow();
		AccumulateCurrentStage(now);
		if (s_cpu_stage_profiler.stage_depth != 0 ||
			s_cpu_stage_profiler.current_stage != CpuStage::Scheduler)
		{
			s_cpu_stage_profiler.totals.unbalanced_samples++;
			FinishCurrentInterval(now, 0, 0, 0, 0, false);
			return;
		}
		s_cpu_stage_profiler.current_stage = CpuStage::EeGenerated;
		s_cpu_stage_profiler.stage_start_us = now;
		s_cpu_stage_profiler.totals.stage_entries[
			static_cast<size_t>(CpuStage::EeGenerated)]++;
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

	void CountCpuStageEntry(CpuStage stage)
	{
		if (!g_cpu_stage_sample_active)
			return;
		s_cpu_stage_profiler.totals.stage_entries[
			static_cast<size_t>(stage)]++;
	}

	void RecordIopDeadlineGate(bool dispatched, bool deadline_due,
		bool counter_due, bool counter_precedes_published, bool intc_visible,
		bool callback_due)
	{
		s_cpu_stage_profiler.totals.iop_deadline_gate_checks++;
		if (dispatched)
			s_cpu_stage_profiler.totals.iop_deadline_gate_dispatches++;
		else
			s_cpu_stage_profiler.totals.iop_deadline_gate_skips++;
		if (counter_precedes_published)
			s_cpu_stage_profiler.totals.iop_deadline_shadow_late++;
		if (deadline_due)
			s_cpu_stage_profiler.totals.iop_deadline_due++;
		if (counter_due)
			s_cpu_stage_profiler.totals.iop_counter_due++;
		if (intc_visible)
			s_cpu_stage_profiler.totals.iop_intc_visible++;
		if (callback_due)
			s_cpu_stage_profiler.totals.iop_callback_due++;
		if (deadline_due && !counter_due && !counter_precedes_published &&
			!intc_visible && !callback_due)
		{
			s_cpu_stage_profiler.totals.iop_manufactured_only++;
		}
	}
#endif
}
