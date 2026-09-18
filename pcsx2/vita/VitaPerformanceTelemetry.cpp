// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaPerformanceTelemetry.h"

#if defined(VITASX2_VIF_EPOCH_CENSUS)
#include "vita/VitaVifEpochCensus.h"
#endif
#if defined(VITASX2_GPU_VU_OPPORTUNITY_CENSUS)
#include "vita/VitaGpuVuOpportunityCensus.h"
#endif

#if defined(VITASX2_CPU_PROFILER)
#include "common/Threading.h"
#include "IopMem.h"
#include "Memory.h"
#include "SPU2/spu2.h"
#endif

#if defined(__vita__) && defined(VITASX2_CPU_PROFILER)
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#endif

#if defined(VITASX2_CPU_PROFILER)
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#endif

namespace VitaPerformanceTelemetry
{
	bool g_enabled = true;
#if defined(VITASX2_CPU_PROFILER)
	bool g_cpu_stage_profiler_enabled = false;
	bool g_cpu_stage_sample_active = false;
	std::atomic<u32> g_cpu_stage_statistical_marker{
		static_cast<u32>(CpuStage::Count)};
	std::atomic<u32> g_cpu_ee_statistical_pc{UINT32_MAX};
	std::atomic<u32> g_cpu_iop_statistical_pc{UINT32_MAX};

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
			u32 ipu_epoch_run_pc = UINT32_MAX;
			u32 ipu_epoch_run_length = 0;
		};
		static_assert(
			sizeof(CpuProfileIntervalRecord) *
				CPU_PROFILE_INTERVAL_RING_SIZE <= 96 * 1024,
			"CPU0 profile interval ring must remain below 96 KiB");
		CpuStageProfilerState s_cpu_stage_profiler;
		std::array<std::atomic<u32>, CPU_STAGE_COUNT>
			s_statistical_stage_samples{};
		std::atomic<u32> s_statistical_samples{0};
		std::atomic<u32> s_statistical_invalid_samples{0};
		constexpr size_t STATISTICAL_GUEST_PC_RING_SIZE = 4096;
		struct StatisticalGuestPcSample
		{
			std::atomic<u64> sequence{0};
			std::atomic<u32> pc{UINT32_MAX};
			std::atomic<u32> block_generation{0};
		};
		std::array<StatisticalGuestPcSample,
			STATISTICAL_GUEST_PC_RING_SIZE> s_statistical_ee_pc_ring{};
		std::array<StatisticalGuestPcSample,
			STATISTICAL_GUEST_PC_RING_SIZE> s_statistical_iop_pc_ring{};
		std::atomic<u64> s_statistical_ee_pc_sequence{0};
		std::atomic<u64> s_statistical_iop_pc_sequence{0};
		// Capturing source words on the sparse sampler thread would race guest
		// memory mutation and could invoke a handler-backed read from a foreign
		// thread. Instead, each CPU compiler publishes its already-decoded words
		// at its cold, source-generation-owned commit seam. The profiler-only
		// registries use bounded open addressing: a retail window can contain more
		// than 20,000 EE blocks, so a direct-mapped 4,096-slot table discarded most
		// of the hot-PC source attribution through ordinary hash collisions.
		// Entries never move or delete during a profile window and no lookup result
		// participates in execution, admission, or source validity.
		constexpr size_t EE_CODE_SNAPSHOT_REGISTRY_SIZE = 32768;
		constexpr size_t IOP_CODE_SNAPSHOT_REGISTRY_SIZE = 4096;
		static_assert(std::has_single_bit(EE_CODE_SNAPSHOT_REGISTRY_SIZE));
		static_assert(std::has_single_bit(IOP_CODE_SNAPSHOT_REGISTRY_SIZE));
		constexpr u32 GUEST_CODE_SNAPSHOT_RESERVED_PC = UINT32_MAX - 1;
		enum EeTraceField : size_t
		{
			EeTraceInstructionCount,
			EeTraceSourceInstructionCount,
			EeTraceDependencyStartPc,
			EeTraceDependencyInstructionCount,
			EeTraceScaledCycles,
			EeTraceEmittedBytes,
			EeTraceHostInstructions,
			EeTraceHelperCalls,
			EeTraceStateLoads,
			EeTraceStateStores,
			EeTraceGuestInteger,
			EeTraceGuestBranches,
			EeTraceGuestMemoryLoads,
			EeTraceGuestMemoryStores,
			EeTraceGuestMmi,
			EeTraceGuestCop0,
			EeTraceGuestCop1,
			EeTraceGuestCop2,
			EeTraceGuestOther,
			EeTraceFlags,
			EeTraceSuccessorCount,
			EeTraceSuccessor0,
			EeTraceSuccessor1,
			EeTraceFieldCount,
		};
		struct GuestCodeSnapshot
		{
			std::atomic<u32> sequence{0};
			std::atomic<u32> pc{UINT32_MAX};
			std::atomic<u32> code_words{0};
			std::array<std::atomic<u32>,
				CPU_PROFILE_CODE_WORD_COUNT> code{};
			std::atomic<u32> ee_trace_generation{0};
			std::array<std::atomic<u32>, EeTraceFieldCount>
				ee_trace_fields{};
			std::atomic<u32> ee_trace_predecessor_count{0};
			std::atomic<u32> ee_trace_predecessor_truncated{0};
			std::array<std::atomic<u32>,
				CPU_PROFILE_EE_TRACE_PREDECESSOR_COUNT>
				ee_trace_predecessors{};
		};
		template <size_t Capacity>
		using GuestCodeSnapshotRegistry =
			std::array<GuestCodeSnapshot, Capacity>;
		GuestCodeSnapshotRegistry<EE_CODE_SNAPSHOT_REGISTRY_SIZE>
			s_ee_code_snapshots{};
		GuestCodeSnapshotRegistry<IOP_CODE_SNAPSHOT_REGISTRY_SIZE>
			s_iop_code_snapshots{};

		template <size_t Capacity>
		constexpr size_t GuestCodeSnapshotIndex(u32 pc)
		{
			return ((pc >> 2) * 2654435761u) &
				(Capacity - 1);
		}

		template <size_t Capacity>
		GuestCodeSnapshot* FindOrClaimGuestCodeSnapshot(
			GuestCodeSnapshotRegistry<Capacity>& registry, u32 pc)
		{
			const size_t first = GuestCodeSnapshotIndex<Capacity>(pc);
			for (size_t probe = 0; probe < Capacity; probe++)
			{
				GuestCodeSnapshot& slot =
					registry[(first + probe) & (Capacity - 1)];
				u32 stored_pc = slot.pc.load(std::memory_order_acquire);
				if (stored_pc == pc)
					return &slot;
				if (stored_pc != UINT32_MAX)
					continue;
				if (slot.pc.compare_exchange_strong(stored_pc,
						GUEST_CODE_SNAPSHOT_RESERVED_PC,
						std::memory_order_acq_rel, std::memory_order_acquire))
				{
					slot.pc.store(pc, std::memory_order_release);
					return &slot;
				}
				if (stored_pc == pc)
					return &slot;
			}
			return nullptr;
		}

		template <size_t Capacity>
		bool ReadGuestCodeSnapshot(
			const GuestCodeSnapshotRegistry<Capacity>& registry,
			u32 pc, u32* code_words,
			std::array<u32, CPU_PROFILE_CODE_WORD_COUNT>* code)
		{
			const size_t first = GuestCodeSnapshotIndex<Capacity>(pc);
			for (size_t probe = 0; probe < Capacity; probe++)
			{
				const GuestCodeSnapshot& slot =
					registry[(first + probe) & (Capacity - 1)];
				const u32 stored_pc = slot.pc.load(std::memory_order_acquire);
				if (stored_pc == UINT32_MAX)
					return false;
				if (stored_pc != pc)
					continue;
				const u32 before =
					slot.sequence.load(std::memory_order_acquire);
				if ((before & 1u) != 0)
					return false;
				const u32 stored_words =
					slot.code_words.load(std::memory_order_relaxed);
				std::array<u32, CPU_PROFILE_CODE_WORD_COUNT> stored_code{};
				for (size_t i = 0; i < stored_code.size(); i++)
				{
					stored_code[i] =
						slot.code[i].load(std::memory_order_relaxed);
				}
				std::atomic_thread_fence(std::memory_order_acquire);
				const u32 after =
					slot.sequence.load(std::memory_order_relaxed);
				if (before != after)
					return false;
				*code_words = stored_words;
				*code = stored_code;
				return true;
			}
			return false;
		}

		u32 ReadEeTraceGeneration(u32 pc)
		{
			const size_t first =
				GuestCodeSnapshotIndex<EE_CODE_SNAPSHOT_REGISTRY_SIZE>(pc);
			for (size_t probe = 0;
				probe < EE_CODE_SNAPSHOT_REGISTRY_SIZE; probe++)
			{
				const GuestCodeSnapshot& slot = s_ee_code_snapshots[
					(first + probe) & (EE_CODE_SNAPSHOT_REGISTRY_SIZE - 1)];
				const u32 stored_pc = slot.pc.load(std::memory_order_acquire);
				if (stored_pc == UINT32_MAX)
					return 0;
				if (stored_pc != pc)
					continue;
				const u32 before =
					slot.sequence.load(std::memory_order_acquire);
				if ((before & 1u) != 0)
					return 0;
				const u32 generation = slot.ee_trace_generation.load(
					std::memory_order_relaxed);
				std::atomic_thread_fence(std::memory_order_acquire);
				const u32 after =
					slot.sequence.load(std::memory_order_relaxed);
				return before == after ? generation : 0;
			}
			return 0;
		}

		template <size_t Capacity>
		void PublishGuestCodeSnapshot(
			GuestCodeSnapshotRegistry<Capacity>& registry,
			u32 pc, const u32* code, u32 code_words,
			const EeTraceBlockRegistration* ee_trace = nullptr)
		{
			if (!g_cpu_stage_profiler_enabled || !code)
				return;
			GuestCodeSnapshot* const selected =
				FindOrClaimGuestCodeSnapshot(registry, pc);
			if (!selected)
				return;
			GuestCodeSnapshot& slot = *selected;
			const u32 sequence =
				slot.sequence.fetch_add(1, std::memory_order_acq_rel);
			const u32 retained_words = std::min<u32>(
				code_words, CPU_PROFILE_CODE_WORD_COUNT);
			slot.code_words.store(retained_words, std::memory_order_relaxed);
			for (size_t i = 0; i < CPU_PROFILE_CODE_WORD_COUNT; i++)
			{
				slot.code[i].store(
					i < retained_words ? code[i] : 0,
					std::memory_order_relaxed);
			}
			if (ee_trace)
			{
				const std::array<u32, EeTraceFieldCount> fields = {{
					ee_trace->instruction_count,
					ee_trace->source_instruction_count,
					ee_trace->dependency_start_pc,
					ee_trace->dependency_instruction_count,
					ee_trace->scaled_cycles,
					ee_trace->emitted_bytes,
					ee_trace->host_instructions,
					ee_trace->helper_calls,
					ee_trace->state_loads,
					ee_trace->state_stores,
					ee_trace->guest_integer,
					ee_trace->guest_branches,
					ee_trace->guest_memory_loads,
					ee_trace->guest_memory_stores,
					ee_trace->guest_mmi,
					ee_trace->guest_cop0,
					ee_trace->guest_cop1,
					ee_trace->guest_cop2,
					ee_trace->guest_other,
					ee_trace->flags,
					std::min<u32>(ee_trace->successor_count,
						CPU_PROFILE_EE_TRACE_SUCCESSOR_COUNT),
					ee_trace->successors[0],
					ee_trace->successors[1],
				}};
				for (size_t i = 0; i < fields.size(); i++)
				{
					slot.ee_trace_fields[i].store(
						fields[i], std::memory_order_relaxed);
				}
				u32 generation =
					slot.ee_trace_generation.load(std::memory_order_relaxed) + 1;
				if (generation == 0)
					generation = 1;
				slot.ee_trace_generation.store(
					generation, std::memory_order_relaxed);
			}
			slot.sequence.store(sequence + 2, std::memory_order_release);
		}

		void PublishEeTracePredecessor(u32 target_pc, u32 source_pc)
		{
			GuestCodeSnapshot* const selected = FindOrClaimGuestCodeSnapshot(
				s_ee_code_snapshots, target_pc);
			if (!selected)
				return;
			GuestCodeSnapshot& slot = *selected;
			const u32 sequence =
				slot.sequence.fetch_add(1, std::memory_order_acq_rel);
			u32 count = std::min<u32>(
				slot.ee_trace_predecessor_count.load(std::memory_order_relaxed),
				CPU_PROFILE_EE_TRACE_PREDECESSOR_COUNT);
			bool duplicate = false;
			for (u32 i = 0; i < count; i++)
			{
				duplicate |= slot.ee_trace_predecessors[i].load(
					std::memory_order_relaxed) == source_pc;
			}
			if (!duplicate && count < CPU_PROFILE_EE_TRACE_PREDECESSOR_COUNT)
			{
				slot.ee_trace_predecessors[count].store(
					source_pc, std::memory_order_relaxed);
				slot.ee_trace_predecessor_count.store(
					count + 1, std::memory_order_relaxed);
			}
			else if (!duplicate)
			{
				slot.ee_trace_predecessor_truncated.store(
					1, std::memory_order_relaxed);
			}
			slot.sequence.store(sequence + 2, std::memory_order_release);
		}

#if defined(__vita__)
		Threading::Thread s_statistical_sampler_thread;
		std::atomic<bool> s_statistical_sampler_shutdown{false};

		void StatisticalSamplerThread()
		{
			// CPU0 owns emulation. MTVU and GS own USER_1/USER_2, but both
			// routinely sleep at ordered producer/consumer boundaries. Keep
			// this diagnostic observer off USER_0 and asleep for >99% of its
			// lifetime. Its exact CPU cost is published with every snapshot.
			const Threading::ThreadHandle self =
				Threading::ThreadHandle::GetForCallingThread();
			(void)self.SetAffinity(1u << 2);

			u32 random_state = 0x6d2b79f5u;
			while (!s_statistical_sampler_shutdown.load(
				std::memory_order_acquire))
			{
				const u32 stage = g_cpu_stage_statistical_marker.load(
					std::memory_order_relaxed);
#if defined(VITASX2_VIF_EPOCH_CENSUS)
				VitaVifEpochCensus::RecordStatisticalSample();
#endif
				s_statistical_samples.fetch_add(1,
					std::memory_order_relaxed);
				if (stage < CPU_STAGE_COUNT)
				{
					s_statistical_stage_samples[stage].fetch_add(
						1, std::memory_order_relaxed);
				}
				else
				{
					s_statistical_invalid_samples.fetch_add(
						1, std::memory_order_relaxed);
				}
				const bool sample_ee =
					stage == static_cast<u32>(CpuStage::EeGenerated);
				const bool sample_iop =
					stage == static_cast<u32>(CpuStage::IopGenerated);
				if (sample_ee || sample_iop)
				{
					std::atomic<u32>& published_pc = sample_ee ?
						g_cpu_ee_statistical_pc :
						g_cpu_iop_statistical_pc;
					std::atomic<u64>& published_sequence = sample_ee ?
						s_statistical_ee_pc_sequence :
						s_statistical_iop_pc_sequence;
					auto& ring = sample_ee ?
						s_statistical_ee_pc_ring :
						s_statistical_iop_pc_ring;
					const u32 pc =
						published_pc.load(std::memory_order_relaxed);
					if (pc != UINT32_MAX)
					{
						const u32 block_generation = sample_ee ?
							ReadEeTraceGeneration(pc) : 0;
						const u64 sequence = published_sequence.fetch_add(
							1, std::memory_order_relaxed) + 1;
						StatisticalGuestPcSample& sample =
							ring[sequence % STATISTICAL_GUEST_PC_RING_SIZE];
						sample.pc.store(pc, std::memory_order_relaxed);
						sample.block_generation.store(
							block_generation, std::memory_order_relaxed);
						sample.sequence.store(sequence,
							std::memory_order_release);
					}
				}

				// A fixed millisecond cadence aliases against periodic device
				// work. Xorshift32 gives a deterministic 1750..3797 us delay,
				// decorrelating samples without allocating or reading a clock.
				random_state ^= random_state << 13;
				random_state ^= random_state >> 17;
				random_state ^= random_state << 5;
				const SceUInt delay_us =
					1750u + (random_state & 2047u);
				sceKernelDelayThread(delay_us);
			}
		}

		void StartStatisticalSampler()
		{
			if (s_statistical_sampler_thread.Joinable())
				return;
			s_statistical_sampler_shutdown.store(
				false, std::memory_order_relaxed);
			s_statistical_sampler_thread.SetStackSize(32 * 1024);
			if (!s_statistical_sampler_thread.Start(
					&StatisticalSamplerThread))
			{
				s_statistical_invalid_samples.fetch_add(
					1, std::memory_order_relaxed);
			}
		}
#endif

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

		void PublishStatisticalStage(CpuStage stage)
		{
			g_cpu_stage_statistical_marker.store(
				static_cast<u32>(stage), std::memory_order_relaxed);
		}
	} // namespace

	void RegisterIopGeneratedBlockCode(
		u32 pc, const u32* code, u32 code_words)
	{
		PublishGuestCodeSnapshot(s_iop_code_snapshots, pc, code, code_words);
	}

	void RegisterEeGeneratedBlockCode(
		u32 pc, const u32* code, u32 code_words)
	{
		PublishGuestCodeSnapshot(s_ee_code_snapshots, pc, code, code_words);
	}

	void RegisterEeGeneratedBlockTrace(const EeTraceBlockRegistration& block,
		const u32* code, u32 code_words)
	{
		PublishGuestCodeSnapshot(s_ee_code_snapshots, block.pc,
			code, code_words, &block);
		const u32 successor_count = std::min<u32>(block.successor_count,
			CPU_PROFILE_EE_TRACE_SUCCESSOR_COUNT);
		for (u32 i = 0; i < successor_count; i++)
		{
			if ((block.successors[i] & 3u) == 0)
				PublishEeTracePredecessor(block.successors[i], block.pc);
		}
	}
#endif

	void SetEnabledBeforeVmStart(bool enabled)
	{
		g_enabled = enabled;
	}

	void ConfigureCpuStageProfilerBeforeVmStart(bool enabled)
	{
#if defined(VITASX2_CPU_PROFILER)
		g_cpu_stage_profiler_enabled = enabled;
#if defined(VITASX2_VIF_EPOCH_CENSUS)
		VitaVifEpochCensus::ResetBeforeVmStart();
#endif
#if defined(VITASX2_GPU_VU_OPPORTUNITY_CENSUS)
		VitaGpuVuOpportunityCensus::ConfigureBeforeVmStart(enabled);
#endif
		g_cpu_stage_sample_active = false;
		s_cpu_stage_profiler = {};
		s_cpu_stage_profiler.scheduler_until_sample =
			CPU_STAGE_SAMPLE_PERIOD;
		s_cpu_stage_profiler.totals.valid = enabled;
		g_cpu_stage_statistical_marker.store(
			static_cast<u32>(CpuStage::Count),
			std::memory_order_relaxed);
		g_cpu_ee_statistical_pc.store(UINT32_MAX,
			std::memory_order_relaxed);
		g_cpu_iop_statistical_pc.store(UINT32_MAX,
			std::memory_order_relaxed);
		s_statistical_samples.store(0, std::memory_order_relaxed);
		s_statistical_invalid_samples.store(0, std::memory_order_relaxed);
		s_statistical_ee_pc_sequence.store(0, std::memory_order_relaxed);
		s_statistical_iop_pc_sequence.store(0, std::memory_order_relaxed);
		for (StatisticalGuestPcSample& sample : s_statistical_ee_pc_ring)
		{
			sample.pc.store(UINT32_MAX, std::memory_order_relaxed);
			sample.block_generation.store(0, std::memory_order_relaxed);
			sample.sequence.store(0, std::memory_order_relaxed);
		}
		for (StatisticalGuestPcSample& sample : s_statistical_iop_pc_ring)
		{
			sample.pc.store(UINT32_MAX, std::memory_order_relaxed);
			sample.block_generation.store(0, std::memory_order_relaxed);
			sample.sequence.store(0, std::memory_order_relaxed);
		}
		const auto clear_code_registry = [](auto& registry) {
			for (GuestCodeSnapshot& slot : registry)
			{
				slot.pc.store(UINT32_MAX, std::memory_order_relaxed);
				slot.code_words.store(0, std::memory_order_relaxed);
				for (std::atomic<u32>& word : slot.code)
					word.store(0, std::memory_order_relaxed);
				slot.ee_trace_generation.store(0, std::memory_order_relaxed);
				for (std::atomic<u32>& field : slot.ee_trace_fields)
					field.store(0, std::memory_order_relaxed);
				slot.ee_trace_predecessor_count.store(
					0, std::memory_order_relaxed);
				slot.ee_trace_predecessor_truncated.store(
					0, std::memory_order_relaxed);
				for (std::atomic<u32>& predecessor :
					slot.ee_trace_predecessors)
				{
					predecessor.store(0, std::memory_order_relaxed);
				}
				slot.sequence.store(0, std::memory_order_relaxed);
			}
		};
		clear_code_registry(s_ee_code_snapshots);
		clear_code_registry(s_iop_code_snapshots);
		for (std::atomic<u32>& samples : s_statistical_stage_samples)
			samples.store(0, std::memory_order_relaxed);
#if defined(__vita__)
		if (enabled)
			StartStatisticalSampler();
#endif
#else
		(void)enabled;
#endif
	}

#if defined(VITASX2_CPU_PROFILER)
	void ShutdownCpuStageProfiler()
	{
#if defined(__vita__)
		if (s_statistical_sampler_thread.Joinable())
		{
			s_statistical_sampler_shutdown.store(
				true, std::memory_order_release);
			s_statistical_sampler_thread.Join();
		}
#endif
	}
#endif

	CpuStageProfilerSnapshot GetCpuStageProfilerSnapshot()
	{
#if defined(VITASX2_CPU_PROFILER)
		CpuStageProfilerSnapshot snapshot =
			s_cpu_stage_profiler.totals;
		snapshot.statistical_samples =
			s_statistical_samples.load(std::memory_order_relaxed);
		snapshot.statistical_invalid_samples =
			s_statistical_invalid_samples.load(
				std::memory_order_relaxed);
		snapshot.statistical_ee_pc_sequence =
			s_statistical_ee_pc_sequence.load(std::memory_order_relaxed);
		snapshot.statistical_iop_pc_sequence =
			s_statistical_iop_pc_sequence.load(std::memory_order_relaxed);
		for (size_t i = 0; i < CPU_STAGE_COUNT; i++)
		{
			snapshot.statistical_stage_samples[i] =
				s_statistical_stage_samples[i].load(
					std::memory_order_relaxed);
		}
#if defined(__vita__)
		if (s_statistical_sampler_thread.Joinable())
		{
			snapshot.statistical_sampler_cpu_us =
				s_statistical_sampler_thread.GetCPUTime();
		}
#endif
		return snapshot;
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
			std::array<u32, CPU_PROFILE_CODE_WORD_COUNT> code_start{};
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
				target->start_pc = start_pc;
				target->end_pc = end_pc;
				target->estimate = inherited + 1;
				target->valid = true;
				target->code_start = ee ?
					record.ee_code_start : record.iop_code_start;
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
						stage_time(CpuStage::EeCompile) +
						stage_time(CpuStage::EeInterpreter) +
						stage_time(CpuStage::Cop1) +
						stage_time(CpuStage::EeHelper) +
						stage_time(CpuStage::EeMemorySlowPath);
				}
				return stage_time(CpuStage::IopGuest) +
					stage_time(CpuStage::IopGenerated) +
					stage_time(CpuStage::IopProvider) +
					stage_time(CpuStage::IopCompile) +
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
				edges[i].code_start = candidates[i].code_start;
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
	static CpuProfileHotPcSnapshot GetCpuProfileHotGuestPcSnapshot(
		u64 first_sequence, u64 next_sequence, u32 first_rank,
		const std::array<StatisticalGuestPcSample,
			STATISTICAL_GUEST_PC_RING_SIZE>& ring,
		const std::atomic<u64>& sequence_source)
	{
		CpuProfileHotPcSnapshot snapshot;
		snapshot.valid = s_cpu_stage_profiler.totals.valid;
		snapshot.first_sequence = first_sequence;
		snapshot.next_sequence = next_sequence;
		if (!snapshot.valid || first_sequence >= next_sequence)
			return snapshot;

		const u64 published =
			sequence_source.load(std::memory_order_acquire);
		next_sequence = std::min(next_sequence, published + 1);
		const u64 retained_first =
			published >= STATISTICAL_GUEST_PC_RING_SIZE ?
				published - STATISTICAL_GUEST_PC_RING_SIZE + 1 : 1;
		if (first_sequence < retained_first)
		{
			snapshot.dropped_samples = retained_first - first_sequence;
			first_sequence = retained_first;
		}

		constexpr size_t CANDIDATE_COUNT =
			CPU_PROFILE_HOT_GUEST_PC_CANDIDATE_COUNT;
		struct Candidate
		{
			u32 pc = 0;
			u32 estimate = 0;
			bool valid = false;
		};
		std::array<Candidate, CANDIDATE_COUNT> candidates{};
		const auto read_pc = [&snapshot, &ring](u64 sequence, u32* pc) {
			const StatisticalGuestPcSample& sample =
				ring[sequence % STATISTICAL_GUEST_PC_RING_SIZE];
			const u64 before =
				sample.sequence.load(std::memory_order_acquire);
			const u32 value = sample.pc.load(std::memory_order_relaxed);
			const u64 after =
				sample.sequence.load(std::memory_order_acquire);
			if (before != sequence || after != sequence ||
				value == UINT32_MAX)
			{
				snapshot.invalid_samples++;
				return false;
			}
			*pc = value;
			return true;
		};
		for (u64 sequence = first_sequence;
			sequence < next_sequence; sequence++)
		{
			u32 pc = 0;
			if (!read_pc(sequence, &pc))
				continue;
			Candidate* empty = nullptr;
			Candidate* least = &candidates[0];
			bool matched = false;
			for (Candidate& candidate : candidates)
			{
				if (candidate.valid && candidate.pc == pc)
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
			const u32 inherited =
				target->valid ? target->estimate : 0;
			target->pc = pc;
			target->estimate = inherited + 1;
			target->valid = true;
		}

		std::array<CpuProfileHotPc, CANDIDATE_COUNT> exact{};
		for (size_t i = 0; i < candidates.size(); i++)
		{
			if (candidates[i].valid)
				exact[i].pc = candidates[i].pc;
		}
		for (u64 sequence = first_sequence;
			sequence < next_sequence; sequence++)
		{
			u32 pc = 0;
			if (!read_pc(sequence, &pc))
				continue;
			for (size_t i = 0; i < candidates.size(); i++)
			{
				if (candidates[i].valid && exact[i].pc == pc)
				{
					exact[i].samples++;
					break;
				}
			}
		}
		const bool ee_registry = &ring == &s_statistical_ee_pc_ring;
		const bool iop_registry = &ring == &s_statistical_iop_pc_ring;
		if (ee_registry || iop_registry)
		{
			for (CpuProfileHotPc& hot_pc : exact)
			{
				if (hot_pc.samples == 0)
					continue;
				if (ee_registry)
				{
					ReadGuestCodeSnapshot(s_ee_code_snapshots, hot_pc.pc,
						&hot_pc.code_words, &hot_pc.code);
				}
				else
				{
					ReadGuestCodeSnapshot(s_iop_code_snapshots, hot_pc.pc,
						&hot_pc.code_words, &hot_pc.code);
				}
			}
		}
		std::sort(exact.begin(), exact.end(),
			[](const CpuProfileHotPc& left,
				const CpuProfileHotPc& right) {
				return left.samples > right.samples;
			});
		if (first_rank < exact.size())
		{
			const size_t retained = std::min<size_t>(snapshot.pcs.size(),
				exact.size() - first_rank);
			std::copy_n(exact.begin() + first_rank, retained,
				snapshot.pcs.begin());
		}
		return snapshot;
	}

	CpuProfileHotPcSnapshot GetCpuProfileHotEePcSnapshot(
		u64 first_sequence, u64 next_sequence, u32 first_rank)
	{
		return GetCpuProfileHotGuestPcSnapshot(first_sequence, next_sequence,
			first_rank,
			s_statistical_ee_pc_ring, s_statistical_ee_pc_sequence);
	}

	CpuProfileHotPcSnapshot GetCpuProfileHotIopPcSnapshot(
		u64 first_sequence, u64 next_sequence, u32 first_rank)
	{
		return GetCpuProfileHotGuestPcSnapshot(first_sequence, next_sequence,
			first_rank,
			s_statistical_iop_pc_ring, s_statistical_iop_pc_sequence);
	}

	CpuProfileEeSampleSnapshot GetCpuProfileEeSampleSnapshot(
		u64 first_sequence, u64 next_sequence, u32 first_sample_index)
	{
		CpuProfileEeSampleSnapshot snapshot;
		snapshot.valid = s_cpu_stage_profiler.totals.valid;
		snapshot.first_sequence = first_sequence;
		snapshot.next_sequence = next_sequence;
		snapshot.first_sample_index = first_sample_index;
		if (!snapshot.valid || first_sequence >= next_sequence)
			return snapshot;

		const u64 published =
			s_statistical_ee_pc_sequence.load(std::memory_order_acquire);
		next_sequence = std::min(next_sequence, published + 1);
		snapshot.next_sequence = next_sequence;
		const u64 retained_first =
			published >= STATISTICAL_GUEST_PC_RING_SIZE ?
				published - STATISTICAL_GUEST_PC_RING_SIZE + 1 : 1;
		if (first_sequence < retained_first)
		{
			snapshot.dropped_samples =
				std::min(next_sequence, retained_first) - first_sequence;
			first_sequence = std::min(next_sequence, retained_first);
		}

		u64 valid_index = 0;
		for (u64 sequence = first_sequence;
			sequence < next_sequence; sequence++)
		{
			const StatisticalGuestPcSample& stored =
				s_statistical_ee_pc_ring[
					sequence % STATISTICAL_GUEST_PC_RING_SIZE];
			const u64 before = stored.sequence.load(std::memory_order_acquire);
			const u32 pc = stored.pc.load(std::memory_order_relaxed);
			const u32 generation = stored.block_generation.load(
				std::memory_order_relaxed);
			const u64 after = stored.sequence.load(std::memory_order_acquire);
			if (before != sequence || after != sequence || pc == UINT32_MAX)
			{
				snapshot.invalid_samples++;
				continue;
			}
			if (valid_index >= first_sample_index &&
				snapshot.sample_count < snapshot.samples.size())
			{
				CpuProfileEeSample& sample =
					snapshot.samples[snapshot.sample_count++];
				sample.sequence = sequence;
				sample.pc = pc;
				sample.block_generation = generation;
			}
			valid_index++;
		}
		snapshot.valid_samples = valid_index;
		return snapshot;
	}

	CpuProfileEeTraceBlock GetCpuProfileEeTraceBlock(
		u32 pc, u32 expected_generation)
	{
		CpuProfileEeTraceBlock result;
		const size_t first =
			GuestCodeSnapshotIndex<EE_CODE_SNAPSHOT_REGISTRY_SIZE>(pc);
		for (size_t probe = 0;
			probe < EE_CODE_SNAPSHOT_REGISTRY_SIZE; probe++)
		{
			const GuestCodeSnapshot& slot = s_ee_code_snapshots[
				(first + probe) & (EE_CODE_SNAPSHOT_REGISTRY_SIZE - 1)];
			const u32 stored_pc = slot.pc.load(std::memory_order_acquire);
			if (stored_pc == UINT32_MAX)
				return result;
			if (stored_pc != pc)
				continue;
			const u32 before = slot.sequence.load(std::memory_order_acquire);
			if ((before & 1u) != 0)
				return result;
			const u32 generation = slot.ee_trace_generation.load(
				std::memory_order_relaxed);
			if (generation == 0 ||
				(expected_generation != 0 && generation != expected_generation))
			{
				return result;
			}
			std::array<u32, EeTraceFieldCount> fields{};
			for (size_t i = 0; i < fields.size(); i++)
			{
				fields[i] = slot.ee_trace_fields[i].load(
					std::memory_order_relaxed);
			}
			const u32 predecessor_count =
				slot.ee_trace_predecessor_count.load(
					std::memory_order_relaxed);
			const u32 predecessor_truncated =
				slot.ee_trace_predecessor_truncated.load(
					std::memory_order_relaxed);
			std::array<u32, CPU_PROFILE_EE_TRACE_PREDECESSOR_COUNT>
				predecessors{};
			for (size_t i = 0; i < predecessors.size(); i++)
			{
				predecessors[i] = slot.ee_trace_predecessors[i].load(
					std::memory_order_relaxed);
			}
			const u32 code_words =
				slot.code_words.load(std::memory_order_relaxed);
			std::array<u32, CPU_PROFILE_CODE_WORD_COUNT> code{};
			for (size_t i = 0; i < code.size(); i++)
				code[i] = slot.code[i].load(std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_acquire);
			const u32 after = slot.sequence.load(std::memory_order_relaxed);
			if (before != after ||
				fields[EeTraceSuccessorCount] >
					CPU_PROFILE_EE_TRACE_SUCCESSOR_COUNT ||
				predecessor_count > CPU_PROFILE_EE_TRACE_PREDECESSOR_COUNT ||
				code_words > CPU_PROFILE_CODE_WORD_COUNT)
			{
				return result;
			}

			result.valid = true;
			result.generation = generation;
			result.block.pc = pc;
			result.block.instruction_count = fields[EeTraceInstructionCount];
			result.block.source_instruction_count =
				fields[EeTraceSourceInstructionCount];
			result.block.dependency_start_pc = fields[EeTraceDependencyStartPc];
			result.block.dependency_instruction_count =
				fields[EeTraceDependencyInstructionCount];
			result.block.scaled_cycles = fields[EeTraceScaledCycles];
			result.block.emitted_bytes = fields[EeTraceEmittedBytes];
			result.block.host_instructions = fields[EeTraceHostInstructions];
			result.block.helper_calls = fields[EeTraceHelperCalls];
			result.block.state_loads = fields[EeTraceStateLoads];
			result.block.state_stores = fields[EeTraceStateStores];
			result.block.guest_integer = fields[EeTraceGuestInteger];
			result.block.guest_branches = fields[EeTraceGuestBranches];
			result.block.guest_memory_loads = fields[EeTraceGuestMemoryLoads];
			result.block.guest_memory_stores = fields[EeTraceGuestMemoryStores];
			result.block.guest_mmi = fields[EeTraceGuestMmi];
			result.block.guest_cop0 = fields[EeTraceGuestCop0];
			result.block.guest_cop1 = fields[EeTraceGuestCop1];
			result.block.guest_cop2 = fields[EeTraceGuestCop2];
			result.block.guest_other = fields[EeTraceGuestOther];
			result.block.flags = fields[EeTraceFlags];
			result.block.successor_count = fields[EeTraceSuccessorCount];
			result.block.successors[0] = fields[EeTraceSuccessor0];
			result.block.successors[1] = fields[EeTraceSuccessor1];
			result.predecessor_count = predecessor_count;
			result.predecessor_truncated = predecessor_truncated != 0;
			result.predecessors = predecessors;
			result.code_words = code_words;
			result.code = code;
			return result;
		}
		return result;
	}

#if defined(VITASX2_QEMU_VALIDATION)
	void RecordEeStatisticalSampleForValidation(u32 pc)
	{
		if (!g_cpu_stage_profiler_enabled || pc == UINT32_MAX)
			return;
		const u64 sequence = s_statistical_ee_pc_sequence.fetch_add(
			1, std::memory_order_relaxed) + 1;
		StatisticalGuestPcSample& sample = s_statistical_ee_pc_ring[
			sequence % STATISTICAL_GUEST_PC_RING_SIZE];
		sample.pc.store(pc, std::memory_order_relaxed);
		sample.block_generation.store(
			ReadEeTraceGeneration(pc), std::memory_order_relaxed);
		sample.sequence.store(sequence, std::memory_order_release);
	}
#endif
#endif

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

		PublishStatisticalStage(CpuStage::Scheduler);
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
		for (size_t i = 0; i < CPU_PROFILE_CODE_WORD_COUNT; i++)
		{
			s_cpu_stage_profiler.current_interval.ee_code_start[i] =
				memRead32(ee_pc + static_cast<u32>(i * sizeof(u32)));
		}
		// Capture code on CPU0 at the same sparse boundary as the EE words.
		// The safe accessor has no MMIO side effects, so hot IOP regions can
		// be classified structurally without reading guest state on the
		// observer or GS thread.
		(void)iopMemSafeReadBytes(iop_pc,
			s_cpu_stage_profiler.current_interval.iop_code_start.data(),
			static_cast<u32>(
				sizeof(s_cpu_stage_profiler.current_interval.iop_code_start)));
		s_cpu_stage_profiler.totals.stage_entries[
			static_cast<size_t>(CpuStage::Scheduler)]++;
	}

	void OnEeSchedulerExitEnabled()
	{
		PublishStatisticalStage(CpuStage::EeGenerated);
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

	u32 BeginExactEeCompileMeasurement()
	{
		return ReadProcessTimeLow();
	}

	void EndExactEeCompileMeasurement(u32 start_us)
	{
		const u32 now = ReadProcessTimeLow();
		s_cpu_stage_profiler.totals.ee_compile_observations++;
		s_cpu_stage_profiler.totals.ee_compile_time_us +=
			static_cast<u32>(now - start_us);
	}

	u32 BeginExactIopCompileMeasurement()
	{
		return ReadProcessTimeLow();
	}

	void EndExactIopCompileMeasurement(u32 start_us)
	{
		const u32 now = ReadProcessTimeLow();
		s_cpu_stage_profiler.totals.iop_compile_observations++;
		s_cpu_stage_profiler.totals.iop_compile_time_us +=
			static_cast<u32>(now - start_us);
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

	void RecordIopCounterUpdate(
		bool spu2_only, u32 spu2_deadline_constraints)
	{
		if (spu2_only)
		{
			s_cpu_stage_profiler.totals.iop_counter_spu2_only_updates++;
			if (spu2_deadline_constraints ==
				SPU2::PeriodicDeadlineConstraintNone)
			{
				s_cpu_stage_profiler.totals
					.iop_spu2_unconstrained_updates++;
			}
			if (spu2_deadline_constraints &
				SPU2::PeriodicDeadlineConstraintIrq)
			{
				s_cpu_stage_profiler.totals
					.iop_spu2_irq_limited_updates++;
			}
			if (spu2_deadline_constraints &
				SPU2::PeriodicDeadlineConstraintDma)
			{
				s_cpu_stage_profiler.totals
					.iop_spu2_dma_limited_updates++;
			}
			if (spu2_deadline_constraints &
				SPU2::PeriodicDeadlineConstraintAutoDma)
			{
				s_cpu_stage_profiler.totals
					.iop_spu2_auto_dma_active_updates++;
			}
		}
		else
			s_cpu_stage_profiler.totals.iop_counter_full_updates++;
	}

	void RecordEeDeadlineHorizon(s64 owner_horizon_delta,
		EeDeadlineOwner owner, u32 ee_event_owner, s32 ee_iop_balance,
		bool timer_enabled, u32 timer_delta, bool iop_rapid)
	{
		CpuStageProfilerSnapshot& totals = s_cpu_stage_profiler.totals;
		totals.ee_deadline_shadow_entries++;
		switch (owner)
		{
			case EeDeadlineOwner::Iop:
				totals.ee_deadline_owner_iop++;
				break;
			case EeDeadlineOwner::EeCounter:
				totals.ee_deadline_owner_counter++;
				break;
			case EeDeadlineOwner::EeEvent:
				totals.ee_deadline_owner_event++;
				if (ee_event_owner <
					totals.ee_deadline_event_owners.size())
				{
					totals.ee_deadline_event_owners[
						ee_event_owner]++;
				}
				break;
			case EeDeadlineOwner::None:
				totals.ee_deadline_owner_none++;
				break;
		}

		if (owner_horizon_delta <= 3072)
		{
			totals.ee_deadline_horizon_le_3072++;
		}
		else
		{
			totals.ee_deadline_horizon_gt_3072++;
			if (owner_horizon_delta > 6144)
				totals.ee_deadline_horizon_gt_6144++;
			if (owner_horizon_delta > 12288)
				totals.ee_deadline_horizon_gt_12288++;
			if (!timer_enabled)
				totals.ee_deadline_owner_beyond_3072_timer_off++;
		}

		if (timer_enabled)
		{
			totals.ee_deadline_timer_enabled++;
			if (timer_delta <= 3072)
				totals.ee_deadline_timer_within_3072++;
		}
		if (iop_rapid)
			totals.ee_deadline_iop_rapid++;
		if (ee_iop_balance > 0)
		{
			totals.ee_iop_balance_positive++;
		}
		else
		{
			totals.ee_iop_balance_nonpositive++;
			if (ee_iop_balance < -3072)
				totals.ee_iop_ahead_gt_3072++;
		}
	}

	void RecordEeSchedulerPath(bool iop_only, bool iop_retained_wait)
	{
		if (iop_only)
			s_cpu_stage_profiler.totals.ee_iop_only_scheduler_entries++;
		else
			s_cpu_stage_profiler.totals.ee_full_scheduler_entries++;
		if (iop_retained_wait)
		{
			s_cpu_stage_profiler.totals
				.iop_retained_wait_scheduler_entries++;
		}
	}

	void RecordJointWaitShadow(u32 ee_wait_origin, bool iop_retained_wait,
		s64 horizon_delta, bool unknown_writer, bool blocked,
		u32 ram_offset, u32 ram_size)
	{
		if (ee_wait_origin == 0)
			return;

		CpuStageProfilerSnapshot& totals = s_cpu_stage_profiler.totals;
		totals.ee_wait_shadow_entries++;
		switch (ee_wait_origin)
		{
			case 1:
				totals.ee_wait_generic_ram++;
				break;
			case 2:
				totals.ee_wait_poll_call_ram++;
				break;
			case 3:
				totals.ee_wait_two_predicate_ram++;
				break;
			case 4:
				totals.ee_wait_retained_unconditional++;
				break;
			case 5:
				totals.ee_wait_gs_csr_vsint++;
				break;
			case 6:
				totals.ee_wait_dmac_chcr_str++;
				break;
			case 7:
				totals.ee_wait_intc_vblank_start_and_ram++;
				break;
			default:
				blocked = true;
				break;
		}

		if (!iop_retained_wait)
			return;

		totals.joint_wait_shadow_entries++;
		if (ram_offset != UINT32_MAX && ram_size != 0)
		{
			totals.joint_wait_ram_certified++;
			totals.joint_wait_last_ram_offset = ram_offset;
			totals.joint_wait_last_ram_size = ram_size;
		}
		if (unknown_writer)
			totals.joint_wait_unknown_writer++;
		if (blocked || horizon_delta <= 0)
			totals.joint_wait_blocked++;
		else if (!unknown_writer)
			totals.joint_wait_qualified++;

		if (horizon_delta > 6144)
		{
			totals.joint_wait_horizon_gt_6144++;
			if (horizon_delta > 12288)
			{
				totals.joint_wait_horizon_gt_12288++;
				if (horizon_delta > 24576)
					totals.joint_wait_horizon_gt_24576++;
			}
		}
	}

	void RecordJointWaitRamWriteOverlap(bool scheduler_active)
	{
		CpuStageProfilerSnapshot& totals = s_cpu_stage_profiler.totals;
		totals.joint_wait_ram_write_overlaps++;
		if (!scheduler_active)
			totals.joint_wait_ram_write_overlaps_outside_scheduler++;
	}

	void RecordJointWaitActivation(u32 scheduled_ee_cycles)
	{
		CpuStageProfilerSnapshot& totals = s_cpu_stage_profiler.totals;
		totals.joint_wait_activations++;
		totals.joint_wait_scheduled_ee_cycles += scheduled_ee_cycles;
	}

	void RecordSilentHsyncFold(
		u32 folded_edges, u32 stop_reason, u32 stop_detail)
	{
		CpuStageProfilerSnapshot& totals = s_cpu_stage_profiler.totals;
		totals.silent_hsync_fold_attempts++;
		totals.silent_hsync_fold_last_stop_detail = stop_detail;
		if (folded_edges != 0)
		{
			totals.silent_hsync_fold_activations++;
			totals.silent_hsync_folded_edges += folded_edges;
			return;
		}

		switch (stop_reason)
		{
			case 0:
				totals.silent_hsync_fold_blocked_control++;
				break;
			case 1:
				totals.silent_hsync_fold_blocked_horizon_due++;
				break;
			case 2:
				totals.silent_hsync_fold_blocked_ee_counter++;
				break;
			case 3:
				totals.silent_hsync_fold_blocked_iop_counter++;
				break;
			case 4:
				totals.silent_hsync_fold_blocked_hsync_due++;
				break;
			case 5:
				totals.silent_hsync_fold_blocked_limit++;
				break;
			case 6:
				totals.silent_hsync_fold_blocked_hsint++;
				break;
		}
	}

	static void FinishIpuEpochOpportunityRun()
	{
		const u32 length = s_cpu_stage_profiler.ipu_epoch_run_length;
		if (length == 0)
			return;

		CpuStageProfilerSnapshot& totals = s_cpu_stage_profiler.totals;
		totals.ipu_epoch_longest_chain =
			std::max(totals.ipu_epoch_longest_chain, length);
		if (length == 1)
			totals.ipu_epoch_chain_length_1++;
		else if (length < 4)
			totals.ipu_epoch_chain_length_2_3++;
		else if (length < 8)
			totals.ipu_epoch_chain_length_4_7++;
		else if (length < 16)
			totals.ipu_epoch_chain_length_8_15++;
		else if (length < 32)
			totals.ipu_epoch_chain_length_16_31++;
		else if (length < 64)
			totals.ipu_epoch_chain_length_32_63++;
		else
			totals.ipu_epoch_chain_length_64_plus++;
		s_cpu_stage_profiler.ipu_epoch_run_pc = UINT32_MAX;
		s_cpu_stage_profiler.ipu_epoch_run_length = 0;
	}

	void RecordIpuEpochOpportunity(bool from_ipu_wait, u32 wait_pc,
		u32 due_ipu_mask, u32 blocker_mask)
	{
		// This is an observational census, not admission. A candidate is one
		// complete scheduler boundary at which the exact EE from-IPU CHCR.STR
		// poll and retained IOP wait are still valid, at least one IPU callback
		// is due, and no other currently visible owner must run. Consecutive
		// candidates at the same EE wait PC are the scheduler passes an exact
		// IPU command epoch could replace while still checking every fence
		// before advancing to the next callback deadline.
		CpuStageProfilerSnapshot& totals = s_cpu_stage_profiler.totals;
		if (!from_ipu_wait)
		{
			FinishIpuEpochOpportunityRun();
			return;
		}

		totals.ipu_epoch_from_ipu_wait_entries++;
		if (due_ipu_mask & 1u)
			totals.ipu_epoch_due_from_ipu++;
		if (due_ipu_mask & 2u)
			totals.ipu_epoch_due_to_ipu++;
		if (due_ipu_mask & 4u)
			totals.ipu_epoch_due_process++;

#define IPU_EPOCH_BLOCKER_COUNTER(flag, counter) \
		if (blocker_mask & (flag)) \
			totals.counter++
		IPU_EPOCH_BLOCKER_COUNTER(
			IpuEpochBlockerIopActive, ipu_epoch_blocked_iop_active);
		IPU_EPOCH_BLOCKER_COUNTER(
			IpuEpochBlockerNoDueIpu, ipu_epoch_blocked_no_due_ipu);
		IPU_EPOCH_BLOCKER_COUNTER(
			IpuEpochBlockerDueNonIpu, ipu_epoch_blocked_due_non_ipu);
		IPU_EPOCH_BLOCKER_COUNTER(
			IpuEpochBlockerEeCounter, ipu_epoch_blocked_ee_counter);
		IPU_EPOCH_BLOCKER_COUNTER(
			IpuEpochBlockerCp0Timer, ipu_epoch_blocked_cp0_timer);
		IPU_EPOCH_BLOCKER_COUNTER(
			IpuEpochBlockerVisibleException,
			ipu_epoch_blocked_visible_exception);
		IPU_EPOCH_BLOCKER_COUNTER(
			IpuEpochBlockerVu, ipu_epoch_blocked_vu);
		IPU_EPOCH_BLOCKER_COUNTER(
			IpuEpochBlockerDmacSuspended,
			ipu_epoch_blocked_dmac_suspended);
		IPU_EPOCH_BLOCKER_COUNTER(
			IpuEpochBlockerInstantDma, ipu_epoch_blocked_instant_dma);
#undef IPU_EPOCH_BLOCKER_COUNTER

		if (blocker_mask != 0)
		{
			FinishIpuEpochOpportunityRun();
			return;
		}

		totals.ipu_epoch_candidate_entries++;
		if (s_cpu_stage_profiler.ipu_epoch_run_length != 0 &&
			s_cpu_stage_profiler.ipu_epoch_run_pc == wait_pc)
		{
			s_cpu_stage_profiler.ipu_epoch_run_length++;
			totals.ipu_epoch_candidate_continuations++;
			return;
		}

		FinishIpuEpochOpportunityRun();
		totals.ipu_epoch_candidate_chains++;
		s_cpu_stage_profiler.ipu_epoch_run_pc = wait_pc;
		s_cpu_stage_profiler.ipu_epoch_run_length = 1;
	}

	void RecordSpu2TimeUpdate(u32 samples)
	{
		CpuStageProfilerSnapshot& totals = s_cpu_stage_profiler.totals;
		totals.spu2_time_update_calls++;
		totals.spu2_time_update_samples += samples;
		if (samples == 0)
			totals.spu2_time_update_zero_samples++;
		else if (samples == 1)
			totals.spu2_time_update_one_sample++;
		else if (samples < 16)
			totals.spu2_time_update_2_to_15_samples++;
		else if (samples < 64)
			totals.spu2_time_update_16_to_63_samples++;
		else
			totals.spu2_time_update_64_plus_samples++;
	}

	void RecordSpu2SyncReason(Spu2SyncReason reason)
	{
		CpuStageProfilerSnapshot& totals = s_cpu_stage_profiler.totals;
		switch (reason)
		{
			case Spu2SyncReason::Periodic:
				totals.spu2_sync_periodic++;
				break;
			case Spu2SyncReason::RegisterRead:
				totals.spu2_sync_register_reads++;
				break;
			case Spu2SyncReason::RegisterWrite:
				totals.spu2_sync_register_writes++;
				break;
			case Spu2SyncReason::Dma:
				totals.spu2_sync_dma++;
				break;
			case Spu2SyncReason::Observer:
				totals.spu2_sync_observers++;
				break;
		}
	}

	void RecordSpu2MixerProbe(u32 active_voices, u32 stopped_voices,
		u32 sliding_voices, u32 noise_voices, u32 modulated_voices,
		u32 fx_enabled_cores, u32 irq_enabled_cores,
		u32 reverb_range_cores, u32 auto_dma_cores,
		u32 equivalent_stopped_cores, u32 silent_reverb_samples,
		u32 silent_reverb_input_rejects,
		u32 silent_reverb_irq_rejects,
		u32 silent_reverb_range_rejects,
		u32 silent_reverb_state_rejects)
	{
		CpuStageProfilerSnapshot& totals = s_cpu_stage_profiler.totals;
		totals.spu2_mixer_probes++;
		totals.spu2_mixer_active_voices += active_voices;
		totals.spu2_mixer_stopped_voices += stopped_voices;
		totals.spu2_mixer_sliding_voices += sliding_voices;
		totals.spu2_mixer_noise_voices += noise_voices;
		totals.spu2_mixer_modulated_voices += modulated_voices;
		totals.spu2_mixer_fx_enabled_cores += fx_enabled_cores;
		totals.spu2_mixer_irq_enabled_cores += irq_enabled_cores;
		totals.spu2_mixer_reverb_range_cores += reverb_range_cores;
		totals.spu2_mixer_auto_dma_cores += auto_dma_cores;
		totals.spu2_mixer_equivalent_stopped_cores +=
			equivalent_stopped_cores;
		totals.spu2_mixer_silent_reverb_samples +=
			silent_reverb_samples;
		totals.spu2_mixer_silent_reverb_input_rejects +=
			silent_reverb_input_rejects;
		totals.spu2_mixer_silent_reverb_irq_rejects +=
			silent_reverb_irq_rejects;
		totals.spu2_mixer_silent_reverb_range_rejects +=
			silent_reverb_range_rejects;
		totals.spu2_mixer_silent_reverb_state_rejects +=
			silent_reverb_state_rejects;
	}

	void RecordSpu2StoppedVoiceBatch(
		u32 samples, u32 stable_core0_mask, u32 stable_core1_mask,
		u32 zero_input_reverb_core_mask)
	{
		CpuStageProfilerSnapshot& totals = s_cpu_stage_profiler.totals;
		totals.spu2_stopped_voice_batch_calls++;
		totals.spu2_stopped_voice_batch_samples += samples;
		totals.spu2_stopped_voice_bulk_voice_samples +=
			samples * static_cast<u32>(
				__builtin_popcount(stable_core0_mask) +
				__builtin_popcount(stable_core1_mask));
		if (zero_input_reverb_core_mask != 0)
			totals.spu2_zero_input_reverb_batch_calls++;
		totals.spu2_zero_input_reverb_batch_core_samples +=
			samples * static_cast<u32>(
				__builtin_popcount(zero_input_reverb_core_mask));
	}
#endif
}
