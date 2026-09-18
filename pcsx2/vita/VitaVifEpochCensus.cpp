// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaVifEpochCensus.h"

#if defined(VITASX2_VIF_EPOCH_CENSUS)

#include "Vif.h"
#include "vita/VitaPerformanceTelemetry.h"

#include <algorithm>
#include <atomic>

namespace VitaVifEpochCensus
{
	bool g_runtime_enabled = false;
	std::atomic<u32> g_active_transfer_mask{0};
	std::atomic<u32> g_statistical_stage{
		static_cast<u32>(StatisticalStage::Count)};

	namespace
	{
		struct ActiveUnit
		{
			bool transfer_active = false;
			bool pending_queue_boundary = false;
			u32 transfer_input_words = 0;
			u32 epoch_commands = 0;
			u32 epoch_words = 0;
		};

		std::array<UnitStatistics, 2> s_statistics{};
		std::array<ActiveUnit, 2> s_active{};
		std::atomic<u64> s_statistical_samples{0};
		std::atomic<u64> s_statistical_unclassified_samples{0};
		std::array<std::atomic<u64>, STATISTICAL_STAGE_COUNT>
			s_statistical_stage_samples{};

		bool Enabled()
		{
			return VitaPerformanceTelemetry::g_cpu_stage_profiler_enabled;
		}

		size_t SizeBucket(u64 value)
		{
			if (value == 0)
				return 0;
			if (value == 1)
				return 1;
			if (value <= 3)
				return 2;
			if (value <= 15)
				return 3;
			if (value <= 63)
				return 4;
			if (value <= 255)
				return 5;
			return 6;
		}

		void FinishEpoch(u32 unit, BoundaryReason reason)
		{
			UnitStatistics& statistics = s_statistics[unit];
			ActiveUnit& active = s_active[unit];
			statistics.boundaries[static_cast<size_t>(reason)]++;
			if (active.epoch_commands == 0 && active.epoch_words == 0)
			{
				statistics.empty_boundaries++;
				return;
			}

			statistics.body_epochs++;
			statistics.body_commands += active.epoch_commands;
			statistics.body_words += active.epoch_words;
			const size_t command_bucket = SizeBucket(active.epoch_commands);
			const size_t word_bucket = SizeBucket(active.epoch_words);
			statistics.epoch_command_counts[command_bucket]++;
			statistics.epoch_command_weight[command_bucket] +=
				active.epoch_commands;
			statistics.epoch_word_counts[word_bucket]++;
			statistics.epoch_word_weight[word_bucket] += active.epoch_words;
			active.epoch_commands = 0;
			active.epoch_words = 0;
		}
	} // namespace

	CommandClass ClassifyCommand(u32 unit, u32 command)
	{
		const u32 opcode = command & 0x7fu;
		if (unit == 0 && (opcode == 0x02u || opcode == 0x03u ||
			opcode == 0x06u || opcode == 0x11u || opcode == 0x13u ||
			opcode == 0x50u || opcode == 0x51u))
		{
			return CommandClass::Error;
		}

		switch (opcode)
		{
			case 0x00:
			case 0x01:
			case 0x02:
			case 0x03:
			case 0x04:
			case 0x05:
			case 0x07:
				return CommandClass::State;
			case 0x20:
			case 0x30:
			case 0x31:
				return CommandClass::StatePayload;
			case 0x06:
			case 0x10:
			case 0x11:
			case 0x13:
			case 0x14:
			case 0x15:
			case 0x17:
				return CommandClass::Synchronize;
			case 0x4a:
				return CommandClass::Mpg;
			case 0x50:
			case 0x51:
				return CommandClass::Direct;
			default:
				break;
		}

		if (opcode >= 0x60u &&
			(opcode & 0x0fu) != 0x03u &&
			(opcode & 0x0fu) != 0x07u &&
			(opcode & 0x0fu) != 0x0bu)
		{
			return CommandClass::Unpack;
		}
		return CommandClass::Error;
	}

	bool IsObservationCommand(CommandClass command_class)
	{
		return command_class == CommandClass::Direct ||
			command_class == CommandClass::Synchronize ||
			command_class == CommandClass::Error;
	}

	void ResetBeforeVmStart()
	{
		g_runtime_enabled =
			VitaPerformanceTelemetry::g_cpu_stage_profiler_enabled;
		g_active_transfer_mask.store(0, std::memory_order_relaxed);
		g_statistical_stage.store(
			static_cast<u32>(StatisticalStage::Count),
			std::memory_order_relaxed);
		s_statistical_samples.store(0, std::memory_order_relaxed);
		s_statistical_unclassified_samples.store(
			0, std::memory_order_relaxed);
		for (std::atomic<u64>& samples : s_statistical_stage_samples)
			samples.store(0, std::memory_order_relaxed);
		s_statistics = {};
		s_active = {};
	}

	void BeginTransfer(u32 unit, u32 input_words, bool tte,
		u32 active_command, u32 active_pass)
	{
		if (!Enabled() || unit >= s_statistics.size())
			return;

		UnitStatistics& statistics = s_statistics[unit];
		ActiveUnit& active = s_active[unit];
		if (active.transfer_active)
		{
			statistics.unbalanced_transfers++;
			FinishEpoch(unit, BoundaryReason::NoProgress);
		}
		active = {};
		active.transfer_active = true;
		g_active_transfer_mask.store(
			g_active_transfer_mask.load(std::memory_order_relaxed) |
				(1u << unit),
			std::memory_order_relaxed);
		const ScopedStatisticalStage accounting_stage(
			StatisticalStage::Accounting);
		active.transfer_input_words = input_words;
		statistics.transfer_calls++;
		statistics.transfer_input_words += input_words;
		statistics.tte_calls += tte ? 1u : 0u;
		const size_t bucket = SizeBucket(input_words);
		statistics.transfer_size_calls[bucket]++;
		statistics.transfer_size_words[bucket] += input_words;

		if (active_command == 0)
		{
			statistics.idle_entries++;
			return;
		}

		statistics.continuation_entries++;
		const CommandClass command_class =
			ClassifyCommand(unit, active_command);
		if (!IsObservationCommand(command_class) && active_pass != 0)
			active.epoch_commands = 1;
	}

	void BeginCommand(u32 unit, CommandClass command_class)
	{
		if (!Enabled() || unit >= s_statistics.size())
			return;
		UnitStatistics& statistics = s_statistics[unit];
		ActiveUnit& active = s_active[unit];
		statistics.commands_started++;
		statistics.command_starts[static_cast<size_t>(command_class)]++;
		if (IsObservationCommand(command_class))
		{
			FinishEpoch(unit, BoundaryReason::CommandObserver);
		}
		else
		{
			active.epoch_commands++;
		}
	}

	void RecordCommandStep(u32 unit, CommandClass command_class,
		u32 consumed_words, bool completed)
	{
		if (!Enabled() || unit >= s_statistics.size())
			return;
		UnitStatistics& statistics = s_statistics[unit];
		ActiveUnit& active = s_active[unit];
		const size_t command_index = static_cast<size_t>(command_class);
		statistics.command_steps[command_index]++;
		statistics.command_words[command_index] += consumed_words;
		const size_t size_bucket = SizeBucket(consumed_words);
		statistics.command_step_counts[size_bucket]++;
		statistics.command_step_words[size_bucket] += consumed_words;
		if (completed)
		{
			statistics.commands_completed++;
			statistics.command_completions[command_index]++;
		}
		if (!IsObservationCommand(command_class))
			active.epoch_words += consumed_words;
		if (active.pending_queue_boundary)
		{
			FinishEpoch(unit, BoundaryReason::QueueExecution);
			active.pending_queue_boundary = false;
		}
	}

	void RecordQueueResult(u32 unit, QueueResult result)
	{
		if (!Enabled() || unit >= s_statistics.size())
			return;
		s_statistics[unit].queue_results[static_cast<size_t>(result)]++;
		if (result == QueueResult::Executed && s_active[unit].transfer_active)
			s_active[unit].pending_queue_boundary = true;
	}

	void EndTransfer(u32 unit, u32 remaining_words, bool stalled,
		u32 stall_reason, bool active_command, bool irq_pending,
		bool made_progress)
	{
		if (!Enabled() || unit >= s_statistics.size())
			return;
		UnitStatistics& statistics = s_statistics[unit];
		ActiveUnit& active = s_active[unit];
		if (!active.transfer_active)
		{
			statistics.unbalanced_transfers++;
			g_active_transfer_mask.store(
				g_active_transfer_mask.load(std::memory_order_relaxed) &
					~(1u << unit),
				std::memory_order_relaxed);
			return;
		}
		statistics.transfer_consumed_words +=
			active.transfer_input_words - std::min(
				active.transfer_input_words, remaining_words);

		if (active.pending_queue_boundary)
		{
			FinishEpoch(unit, BoundaryReason::QueueExecution);
			active.pending_queue_boundary = false;
		}

		BoundaryReason reason = BoundaryReason::InputComplete;
		if (stalled && stall_reason == VIF_IRQ_STALL)
			reason = BoundaryReason::Irq;
		else if (stalled)
			reason = BoundaryReason::TimingStall;
		else if (irq_pending && remaining_words != 0)
			reason = BoundaryReason::Irq;
		else if (active_command)
			reason = BoundaryReason::PartialCommand;
		else if (!made_progress)
			reason = BoundaryReason::NoProgress;

		FinishEpoch(unit, reason);
		active = {};
		g_active_transfer_mask.store(
			g_active_transfer_mask.load(std::memory_order_relaxed) &
				~(1u << unit),
			std::memory_order_relaxed);
	}

	void RecordStatisticalSample()
	{
		if (!g_runtime_enabled ||
			g_active_transfer_mask.load(std::memory_order_relaxed) == 0)
		{
			return;
		}

		s_statistical_samples.fetch_add(1, std::memory_order_relaxed);
		const u32 stage =
			g_statistical_stage.load(std::memory_order_relaxed);
		if (stage < STATISTICAL_STAGE_COUNT)
		{
			s_statistical_stage_samples[stage].fetch_add(
				1, std::memory_order_relaxed);
		}
		else
		{
			s_statistical_unclassified_samples.fetch_add(
				1, std::memory_order_relaxed);
		}
	}

	Snapshot GetSnapshot()
	{
		Snapshot snapshot;
		snapshot.valid = Enabled();
		if (snapshot.valid)
		{
			snapshot.statistical_samples =
				s_statistical_samples.load(std::memory_order_relaxed);
			snapshot.statistical_unclassified_samples =
				s_statistical_unclassified_samples.load(
					std::memory_order_relaxed);
			for (size_t i = 0; i < STATISTICAL_STAGE_COUNT; i++)
			{
				snapshot.statistical_stage_samples[i] =
					s_statistical_stage_samples[i].load(
						std::memory_order_relaxed);
			}
			snapshot.units = s_statistics;
		}
		return snapshot;
	}
} // namespace VitaVifEpochCensus

#endif
