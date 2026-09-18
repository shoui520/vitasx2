// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <array>
#include <cstddef>
#if defined(VITASX2_VIF_EPOCH_CENSUS)
#include <atomic>
#endif

namespace VitaVifEpochCensus
{
	enum class CommandClass : u8
	{
		State,
		StatePayload,
		Unpack,
		Mpg,
		Direct,
		Synchronize,
		Error,
		Count,
	};

	enum class BoundaryReason : u8
	{
		InputComplete,
		PartialCommand,
		Irq,
		TimingStall,
		CommandObserver,
		QueueExecution,
		NoProgress,
		Count,
	};

	enum class QueueResult : u8
	{
		Empty,
		BlockedVu,
		BlockedGif,
		Executed,
		Count,
	};

	// This is deliberately independent of VitaPerformanceTelemetry::CpuStage.
	// The epoch stages overlap the existing mutually exclusive VIF transfer and
	// unpack stages, so adding them to CpuStage would both double-count the VIF
	// owner and enlarge every sparse interval-ring record.
	enum class StatisticalStage : u8
	{
		Parse,
		State,
		StatePayload,
		Unpack,
		Mpg,
		Direct,
		Synchronize,
		Error,
		CommandBookkeeping,
		DmaBookkeeping,
		Stall,
		QueueProbe,
		QueueObserver,
		QueueExecution,
		Accounting,
		Count,
	};

	static constexpr size_t COMMAND_CLASS_COUNT =
		static_cast<size_t>(CommandClass::Count);
	static constexpr size_t BOUNDARY_REASON_COUNT =
		static_cast<size_t>(BoundaryReason::Count);
	static constexpr size_t QUEUE_RESULT_COUNT =
		static_cast<size_t>(QueueResult::Count);
	static constexpr size_t STATISTICAL_STAGE_COUNT =
		static_cast<size_t>(StatisticalStage::Count);
	static constexpr size_t SIZE_BUCKET_COUNT = 7;

	struct UnitStatistics
	{
		u64 transfer_calls = 0;
		u64 transfer_input_words = 0;
		u64 transfer_consumed_words = 0;
		u64 tte_calls = 0;
		u64 idle_entries = 0;
		u64 continuation_entries = 0;
		u64 commands_started = 0;
		u64 commands_completed = 0;
		u64 body_epochs = 0;
		u64 body_commands = 0;
		u64 body_words = 0;
		u64 empty_boundaries = 0;
		u64 unbalanced_transfers = 0;
		std::array<u64, COMMAND_CLASS_COUNT> command_starts{};
		std::array<u64, COMMAND_CLASS_COUNT> command_steps{};
		std::array<u64, COMMAND_CLASS_COUNT> command_completions{};
		std::array<u64, COMMAND_CLASS_COUNT> command_words{};
		std::array<u64, BOUNDARY_REASON_COUNT> boundaries{};
		std::array<u64, QUEUE_RESULT_COUNT> queue_results{};
		std::array<u64, SIZE_BUCKET_COUNT> transfer_size_calls{};
		std::array<u64, SIZE_BUCKET_COUNT> transfer_size_words{};
		std::array<u64, SIZE_BUCKET_COUNT> epoch_command_counts{};
		std::array<u64, SIZE_BUCKET_COUNT> epoch_command_weight{};
		std::array<u64, SIZE_BUCKET_COUNT> epoch_word_counts{};
		std::array<u64, SIZE_BUCKET_COUNT> epoch_word_weight{};
		std::array<u64, SIZE_BUCKET_COUNT> command_step_counts{};
		std::array<u64, SIZE_BUCKET_COUNT> command_step_words{};
	};

	struct Snapshot
	{
		bool valid = false;
		u64 statistical_samples = 0;
		u64 statistical_unclassified_samples = 0;
		std::array<u64, STATISTICAL_STAGE_COUNT>
			statistical_stage_samples{};
		std::array<UnitStatistics, 2> units{};
	};

#if defined(VITASX2_VIF_EPOCH_CENSUS)
	extern bool g_runtime_enabled;
	extern std::atomic<u32> g_active_transfer_mask;
	extern std::atomic<u32> g_statistical_stage;

	class ScopedStatisticalStage
	{
	public:
		explicit ScopedStatisticalStage(StatisticalStage stage)
		{
			m_entered = g_runtime_enabled &&
				g_active_transfer_mask.load(std::memory_order_relaxed) != 0;
			if (m_entered)
			{
				m_previous_stage = g_statistical_stage.load(
					std::memory_order_relaxed);
				g_statistical_stage.store(static_cast<u32>(stage),
					std::memory_order_relaxed);
			}
		}

		~ScopedStatisticalStage()
		{
			if (m_entered)
			{
				g_statistical_stage.store(
					m_previous_stage, std::memory_order_relaxed);
			}
		}

		ScopedStatisticalStage(const ScopedStatisticalStage&) = delete;
		ScopedStatisticalStage& operator=(
			const ScopedStatisticalStage&) = delete;

	private:
		bool m_entered = false;
		u32 m_previous_stage = static_cast<u32>(StatisticalStage::Count);
	};

	CommandClass ClassifyCommand(u32 unit, u32 command);
	bool IsObservationCommand(CommandClass command_class);
	void ResetBeforeVmStart();
	void BeginTransfer(u32 unit, u32 input_words, bool tte,
		u32 active_command, u32 active_pass);
	void BeginCommand(u32 unit, CommandClass command_class);
	void RecordCommandStep(u32 unit, CommandClass command_class,
		u32 consumed_words, bool completed);
	void RecordQueueResult(u32 unit, QueueResult result);
	void EndTransfer(u32 unit, u32 remaining_words, bool stalled,
		u32 stall_reason, bool active_command, bool irq_pending,
		bool made_progress);
	void RecordStatisticalSample();
	Snapshot GetSnapshot();
#endif
} // namespace VitaVifEpochCensus
