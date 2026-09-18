// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <array>
#include <cstddef>

namespace VitaVU
{
	struct GpuPairPlan;
}

namespace VitaGpuVu
{
	struct VifUnpackSpan;
}

namespace VitaGpuVuOpportunityCensus
{
	inline constexpr size_t PairKindCount = 128;
	inline constexpr size_t SizeBucketCount = 7;
	inline constexpr size_t ProgramCapacity = 256;

	// A current-fixed-GXP preflight candidate is not an admitted or proven
	// exact epoch. These title-agnostic reasons explain which semantic,
	// command, state, or output boundary prevents an actually executed CPU VU1
	// job from even becoming a universal replay candidate.
	enum class UniversalPreflightRejection : u8
	{
		ProgramEncoding,
		IncompleteExecution,
		ResumeExecution,
		EmptyExecution,
		InvalidPairPlan,
		UnsupportedConfiguration,
		UnsupportedNumericConfiguration,
		InvalidPairMetadata,
		UnsupportedUpperBody,
		UnsupportedLowerBody,
		ApproximateQArithmetic,
		PipelineState,
		EnabledDbit,
		EnabledTbit,
		PairCapacity,
		CommandCapacity,
		PayloadCapacity,
		UnsupportedVifUnpack,
		MissingTerminalEbit,
		Path1PacketCapacity,
		Path1DataCapacity,
		TerminalDelayedXgkick,
		DelayedFmacFlagRead,
		Count,
	};
	inline constexpr size_t UniversalPreflightRejectionCount =
		static_cast<size_t>(UniversalPreflightRejection::Count);
	static_assert(UniversalPreflightRejectionCount <= 64);

	struct ProgramStatistics
	{
		u64 executed_slice_hash = 0;
		u32 start_pc = 0;
		bool resume = false;
		u64 jobs = 0;
		u64 completed_jobs = 0;
		u64 cpu_us = 0;
		u64 vu_cycles = 0;
		u64 executed_pairs = 0;
		u64 pairplan_pairs = 0;
		u64 fixed_shader_body_pairs = 0;
		u64 observation_free_pairs = 0;
		u64 distinct_pairs = 0;
		u64 xgkick_pairs = 0;
		u64 path1_bytes = 0;
		u64 unpack_commands = 0;
		u64 unpack_vectors = 0;
		u64 unpack_payload_bytes = 0;
		u64 preflight_candidate_jobs = 0;
		u64 preflight_candidate_pairs = 0;
		u64 preflight_rejection_mask = 0;
	};

	struct Snapshot
	{
		bool valid = false;
		u64 execute_jobs = 0;
		u64 explicit_jobs = 0;
		u64 resume_jobs = 0;
		u64 completed_jobs = 0;
		u64 still_active_jobs = 0;
		u64 cpu_us = 0;
		u64 vu_cycles = 0;
		u64 executed_blocks = 0;
		u64 executed_pairs = 0;
		u64 pairplan_pairs = 0;
		u64 invalid_pairplan_pairs = 0;
		u64 fixed_shader_body_pairs = 0;
		u64 observation_free_pairs = 0;
		u64 fully_fixed_body_jobs = 0;
		u64 fully_observation_free_jobs = 0;
		u64 interpreter_pairs = 0;
		u64 malformed_block_reports = 0;
		u64 branch_pairs = 0;
		u64 indirect_branch_pairs = 0;
		u64 xgkick_pairs = 0;
		u64 ebit_pairs = 0;
		u64 mbit_pairs = 0;
		u64 enabled_dbit_pairs = 0;
		u64 enabled_tbit_pairs = 0;
		u64 clip_pairs = 0;
		u64 fdiv_pairs = 0;
		u64 efu_pairs = 0;
		u64 vif_epochs = 0;
		u64 vif_unpack_commands = 0;
		u64 vif_unpack_vectors = 0;
		u64 vif_unpack_payload_bytes = 0;
		u64 serializable_unpack_commands = 0;
		u64 fixed_shader_unpack_commands = 0;
		u64 fully_serializable_vif_epochs = 0;
		u64 fully_fixed_shader_vif_epochs = 0;
		u64 row_state_updates = 0;
		u64 column_state_updates = 0;
		u64 micro_writes = 0;
		u64 micro_write_bytes = 0;
		u64 data_writes = 0;
		u64 data_write_bytes = 0;
		u64 vi_state_writes = 0;
		u64 vf_state_writes = 0;
		u64 path1_jobs = 0;
		u64 path1_bytes = 0;
		u64 preflight_candidate_jobs = 0;
		u64 preflight_candidate_pairs = 0;
		std::array<u64, UniversalPreflightRejectionCount>
			preflight_rejection_jobs{};
		std::array<u64, UniversalPreflightRejectionCount>
			preflight_rejection_pair_weight{};
		u64 dropped_program_keys = 0;
		u64 dropped_program_pair_weight = 0;
		std::array<u64, PairKindCount> upper_kind_pairs{};
		std::array<u64, PairKindCount> lower_kind_pairs{};
		std::array<u64, SizeBucketCount> job_pair_counts{};
		std::array<u64, SizeBucketCount> job_pair_weight{};
		std::array<u64, SizeBucketCount> epoch_unpack_counts{};
		std::array<u64, SizeBucketCount> epoch_unpack_weight{};
		std::array<u64, SizeBucketCount> epoch_payload_counts{};
		std::array<u64, SizeBucketCount> epoch_payload_weight{};
		std::array<ProgramStatistics, ProgramCapacity> programs{};
		u32 program_count = 0;
	};

#if defined(VITASX2_GPU_VU_OPPORTUNITY_CENSUS)
	// Configured before the VM and worker threads start. This diagnostic never
	// changes VU semantics; IsEnabled() is also used by the A32 provider to keep
	// each natural block observable to the census instead of direct-linking it.
	void ConfigureBeforeVmStart(bool enabled);
	bool IsEnabled();

	void RecordVifUnpack(const VitaGpuVu::VifUnpackSpan& span);
	void RecordRowStateUpdate();
	void RecordColumnStateUpdate();
	void RecordMicroWrite(u32 bytes);
	void RecordDataWrite(u32 bytes);
	void RecordViStateWrite();
	void RecordVfStateWrite();

	void BeginVuExecute(const u8* micro, u32 micro_size, u32 start_pc,
		bool resume, u32 fbrst);
	// With census direct links disabled, one provider return describes one
	// contiguous natural block. The bytes are decoded again through the owning
	// PairPlan analyzer so census logic never becomes a second VU decoder.
	void RecordExecutedBlock(const u8* pair_bytes, u32 start_pc,
		u32 executed_pairs, bool interpreter);
	void EndVuExecute(u64 cpu_us, u64 vu_cycles, u32 path1_bytes,
		bool program_active);

	Snapshot GetSnapshot();
	const char* UpperKindName(u32 kind);
	const char* LowerKindName(u32 kind);
	const char* UniversalPreflightRejectionName(
		UniversalPreflightRejection rejection);
#endif
} // namespace VitaGpuVuOpportunityCensus
