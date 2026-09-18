// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuOpportunityCensus.h"

#include "vita/VitaGpuVuArchitectureLedger.h"

#if defined(VITASX2_GPU_VU_OPPORTUNITY_CENSUS)

#include "VUmicro.h"
#include "VUmicroFast.h"
#include "common/Threading.h"
#include "vita/VitaGpuVuCommandEpoch.h"
#include "vita/VitaGpuVuMicroProgram.h"
#include "vita/VitaGpuVuVifInput.h"
#include "vita/VitaVuBlockCompiler.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>

namespace VitaGpuVuOpportunityCensus
{
	namespace
	{
		using LowerKind = VUInterpFast::LowerFastKind;
		using UpperKind = VUInterpFast::UpperFastKind;

		struct PendingVifEpoch
		{
			u64 unpack_commands = 0;
			u64 unpack_vectors = 0;
			u64 unpack_payload_bytes = 0;
			u64 serializable_unpack_commands = 0;
			u64 fixed_shader_unpack_commands = 0;
			u64 row_state_updates = 0;
			u64 column_state_updates = 0;
			u64 micro_writes = 0;
			u64 micro_write_bytes = 0;
			u64 data_writes = 0;
			u64 data_write_bytes = 0;
			u64 vi_state_writes = 0;
			u64 vf_state_writes = 0;
		};

		struct ActiveExecution
		{
			bool active = false;
			bool resume = false;
			bool all_pairplan = true;
			bool all_fixed_shader_body = true;
			bool all_observation_free = true;
			const u8* micro = nullptr;
			u32 micro_size = 0;
			u32 start_pc = 0;
			u32 fbrst = 0;
			u32 configuration_bits = 0;
			u64 preflight_rejection_mask = 0;
			bool pending_xgkick = false;
			VitaGpuVu::UniversalMicroProgramHandle program;
			VitaGpuVu::UniversalFixedPipelineState pipeline;
			u64 executed_blocks = 0;
			u64 executed_pairs = 0;
			u64 pairplan_pairs = 0;
			u64 invalid_pairplan_pairs = 0;
			u64 fixed_shader_body_pairs = 0;
			u64 observation_free_pairs = 0;
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
			std::array<u64, PairKindCount> upper_kind_pairs{};
			std::array<u64, PairKindCount> lower_kind_pairs{};
			std::array<u64, VU1_PROGSIZE / 8 / 64> seen_pairs{};
			PendingVifEpoch vif;
		};

		Threading::KernelMutex s_mutex;
		bool s_enabled = false;
		Snapshot s_statistics;
		PendingVifEpoch s_pending_vif;
		ActiveExecution s_active;

		constexpr u64 RejectionBit(UniversalPreflightRejection rejection)
		{
			return 1ull << static_cast<u8>(rejection);
		}

		void Reject(UniversalPreflightRejection rejection)
		{
			s_active.preflight_rejection_mask |= RejectionBit(rejection);
		}

		void RejectPairSupport(VitaGpuVu::UniversalFixedPairSupport support)
		{
			switch (support)
			{
				case VitaGpuVu::UniversalFixedPairSupport::Supported:
					return;
				case VitaGpuVu::UniversalFixedPairSupport::UnsupportedConfiguration:
					Reject(UniversalPreflightRejection::UnsupportedConfiguration);
					return;
				case VitaGpuVu::UniversalFixedPairSupport::UnsupportedNumericConfiguration:
					Reject(UniversalPreflightRejection::UnsupportedNumericConfiguration);
					return;
				case VitaGpuVu::UniversalFixedPairSupport::InvalidMetadata:
					Reject(UniversalPreflightRejection::InvalidPairMetadata);
					return;
				case VitaGpuVu::UniversalFixedPairSupport::UnsupportedUpperBody:
					Reject(UniversalPreflightRejection::UnsupportedUpperBody);
					return;
				case VitaGpuVu::UniversalFixedPairSupport::UnsupportedLowerBody:
					Reject(UniversalPreflightRejection::UnsupportedLowerBody);
					return;
				case VitaGpuVu::UniversalFixedPairSupport::ApproximateQArithmetic:
					Reject(UniversalPreflightRejection::ApproximateQArithmetic);
					return;
				case VitaGpuVu::UniversalFixedPairSupport::Count:
					break;
			}
			Reject(UniversalPreflightRejection::InvalidPairMetadata);
		}

		constexpr u64 FnvOffset = 14695981039346656037ull;
		constexpr u64 FnvPrime = 1099511628211ull;

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

		void HashByte(u64* hash, u8 value)
		{
			*hash ^= value;
			*hash *= FnvPrime;
		}

		void HashWord(u64* hash, u32 value)
		{
			for (u32 shift = 0; shift < 32; shift += 8)
				HashByte(hash, static_cast<u8>(value >> shift));
		}

		u32 ReadWord(const u8* source)
		{
			u32 value = 0;
			std::memcpy(&value, source, sizeof(value));
			return value;
		}

		bool IsBranchKind(LowerKind kind)
		{
			return kind >= LowerKind::IBEQ && kind <= LowerKind::JALR;
		}

		bool IsIndirectBranchKind(LowerKind kind)
		{
			return kind == LowerKind::JR || kind == LowerKind::JALR;
		}

		bool IsFdivKind(LowerKind kind)
		{
			return kind == LowerKind::DIV || kind == LowerKind::SQRT ||
				kind == LowerKind::RSQRT || kind == LowerKind::WAITQ;
		}

		bool IsEfuKind(LowerKind kind)
		{
			return kind == LowerKind::WAITP ||
				(kind >= LowerKind::ESADD && kind <= LowerKind::EEXP);
		}

		bool FixedShaderUnpack(const VitaGpuVu::VifUnpackSpan& span)
		{
			return VitaGpuVu::IsFixedUniversalVifUnpackSupported(span);
		}

		void AddPendingToStatistics(const PendingVifEpoch& pending)
		{
			s_statistics.vif_epochs++;
			s_statistics.vif_unpack_commands += pending.unpack_commands;
			s_statistics.vif_unpack_vectors += pending.unpack_vectors;
			s_statistics.vif_unpack_payload_bytes +=
				pending.unpack_payload_bytes;
			s_statistics.serializable_unpack_commands +=
				pending.serializable_unpack_commands;
			s_statistics.fixed_shader_unpack_commands +=
				pending.fixed_shader_unpack_commands;
			s_statistics.fully_serializable_vif_epochs +=
				pending.serializable_unpack_commands == pending.unpack_commands ? 1u : 0u;
			s_statistics.fully_fixed_shader_vif_epochs +=
				pending.fixed_shader_unpack_commands == pending.unpack_commands ? 1u : 0u;
			s_statistics.row_state_updates += pending.row_state_updates;
			s_statistics.column_state_updates += pending.column_state_updates;
			s_statistics.micro_writes += pending.micro_writes;
			s_statistics.micro_write_bytes += pending.micro_write_bytes;
			s_statistics.data_writes += pending.data_writes;
			s_statistics.data_write_bytes += pending.data_write_bytes;
			s_statistics.vi_state_writes += pending.vi_state_writes;
			s_statistics.vf_state_writes += pending.vf_state_writes;

			const size_t command_bucket = SizeBucket(pending.unpack_commands);
			const size_t payload_bucket = SizeBucket(pending.unpack_payload_bytes);
			s_statistics.epoch_unpack_counts[command_bucket]++;
			s_statistics.epoch_unpack_weight[command_bucket] +=
				pending.unpack_commands;
			s_statistics.epoch_payload_counts[payload_bucket]++;
			s_statistics.epoch_payload_weight[payload_bucket] +=
				pending.unpack_payload_bytes;
		}

		u64 ExecutedSliceHash(const ActiveExecution& active, u64* distinct_pairs)
		{
			u64 hash = FnvOffset;
			HashWord(&hash, active.start_pc);
			HashWord(&hash, active.resume ? 1u : 0u);
			u64 distinct = 0;
			if (active.micro && active.micro_size == VU1_PROGSIZE)
			{
				for (u32 pair = 0; pair < VU1_PROGSIZE / 8; pair++)
				{
					if ((active.seen_pairs[pair / 64] &
							(1ull << (pair & 63))) == 0)
					{
						continue;
					}
					distinct++;
					HashWord(&hash, pair * 8);
					HashWord(&hash, ReadWord(active.micro + pair * 8));
					HashWord(&hash, ReadWord(active.micro + pair * 8 + 4));
				}
			}
			*distinct_pairs = distinct;
			return hash;
		}

		ProgramStatistics* FindOrInsertProgram(u64 hash, u32 start_pc,
			bool resume, u64 pair_weight)
		{
			for (u32 index = 0; index < s_statistics.program_count; index++)
			{
				ProgramStatistics& program = s_statistics.programs[index];
				if (program.executed_slice_hash == hash &&
					program.start_pc == start_pc && program.resume == resume)
				{
					return &program;
				}
			}
			if (s_statistics.program_count >= ProgramCapacity)
			{
				s_statistics.dropped_program_keys++;
				s_statistics.dropped_program_pair_weight += pair_weight;
				return nullptr;
			}
			ProgramStatistics& program =
				s_statistics.programs[s_statistics.program_count++];
			program.executed_slice_hash = hash;
			program.start_pc = start_pc;
			program.resume = resume;
			return &program;
		}

		void RecordPlan(const VitaVU::GpuPairPlan& plan,
			VitaGpuVu::UniversalFixedPairSupport support)
		{
			ActiveExecution& active = s_active;
			active.pairplan_pairs++;
			if (plan.upper_kind < PairKindCount)
				active.upper_kind_pairs[plan.upper_kind]++;
			if (plan.lower_kind < PairKindCount)
				active.lower_kind_pairs[plan.lower_kind]++;

			const LowerKind lower = static_cast<LowerKind>(plan.lower_kind);
			const bool branch = plan.exec_lower && IsBranchKind(lower);
			const bool indirect = plan.exec_lower && IsIndirectBranchKind(lower);
			const bool xgkick = plan.exec_lower && lower == LowerKind::XGKICK;
			const bool enabled_d = plan.dflag && (active.fbrst & 0x400u) != 0;
			const bool enabled_t = plan.tflag && (active.fbrst & 0x800u) != 0;
			const bool fixed_body =
				support == VitaGpuVu::UniversalFixedPairSupport::Supported;
			const bool observation_free = fixed_body && !enabled_d && !enabled_t;

			active.branch_pairs += branch ? 1u : 0u;
			active.indirect_branch_pairs += indirect ? 1u : 0u;
			active.xgkick_pairs += xgkick ? 1u : 0u;
			active.ebit_pairs += plan.ebit ? 1u : 0u;
			active.mbit_pairs += plan.mflag ? 1u : 0u;
			active.enabled_dbit_pairs += enabled_d ? 1u : 0u;
			active.enabled_tbit_pairs += enabled_t ? 1u : 0u;
			active.clip_pairs +=
				(plan.exec_upper && plan.upper_kind ==
					static_cast<u32>(UpperKind::CLIP)) ? 1u : 0u;
			active.fdiv_pairs +=
				(plan.exec_lower && IsFdivKind(lower)) ? 1u : 0u;
			active.efu_pairs +=
				(plan.exec_lower && IsEfuKind(lower)) ? 1u : 0u;
			active.fixed_shader_body_pairs += fixed_body ? 1u : 0u;
			active.observation_free_pairs += observation_free ? 1u : 0u;
			active.all_fixed_shader_body &= fixed_body;
			active.all_observation_free &= observation_free;
		}
	} // namespace

	void ConfigureBeforeVmStart(bool enabled)
	{
		std::lock_guard lock(s_mutex);
		s_enabled = enabled;
		s_statistics = {};
		s_statistics.valid = enabled;
		s_pending_vif = {};
		s_active = {};
	}

	bool IsEnabled()
	{
		return s_enabled;
	}

	void RecordVifUnpack(const VitaGpuVu::VifUnpackSpan& span)
	{
		if (!s_enabled)
			return;
		s_pending_vif.unpack_commands++;
		s_pending_vif.unpack_vectors += span.vector_count;
		s_pending_vif.unpack_payload_bytes += span.source_size;
		VitaGpuVu::UniversalEpochMicroOp command;
		if (VitaGpuVu::EncodeUniversalVifUnpackCommand(
				span, 0, &command))
		{
			s_pending_vif.serializable_unpack_commands++;
		}
		if (FixedShaderUnpack(span))
			s_pending_vif.fixed_shader_unpack_commands++;
	}

	void RecordRowStateUpdate()
	{
		if (s_enabled)
			s_pending_vif.row_state_updates++;
	}

	void RecordColumnStateUpdate()
	{
		if (s_enabled)
			s_pending_vif.column_state_updates++;
	}

	void RecordMicroWrite(u32 bytes)
	{
		if (!s_enabled)
			return;
		s_pending_vif.micro_writes++;
		s_pending_vif.micro_write_bytes += bytes;
	}

	void RecordDataWrite(u32 bytes)
	{
		if (!s_enabled)
			return;
		s_pending_vif.data_writes++;
		s_pending_vif.data_write_bytes += bytes;
	}

	void RecordViStateWrite()
	{
		if (s_enabled)
			s_pending_vif.vi_state_writes++;
	}

	void RecordVfStateWrite()
	{
		if (s_enabled)
			s_pending_vif.vf_state_writes++;
	}

	void BeginVuExecute(const u8* micro, u32 micro_size, u32 start_pc,
		bool resume, u32 fbrst)
	{
		if (!s_enabled)
			return;
		// A nested Begin indicates an instrumentation error, not guest behavior.
		if (s_active.active)
		{
			std::lock_guard lock(s_mutex);
			s_statistics.malformed_block_reports++;
		}
		s_active = {};
		s_active.active = true;
		s_active.resume = resume;
		s_active.micro = micro;
		s_active.micro_size = micro_size;
		s_active.start_pc = start_pc & VU1_PROGMASK;
		s_active.fbrst = fbrst;
		s_active.vif = s_pending_vif;
		s_pending_vif = {};
		std::string error;
		s_active.program = VitaGpuVu::PrepareUniversalMicroProgram(
			micro, micro_size, s_active.start_pc, &error);
		if (s_active.program)
		{
			s_active.configuration_bits =
				s_active.program->configuration_bits;
		}
		else
		{
			Reject(UniversalPreflightRejection::ProgramEncoding);
		}
	}

	void RecordExecutedBlock(const u8* pair_bytes, u32 start_pc,
		u32 executed_pairs, bool interpreter)
	{
		if (!s_enabled || !s_active.active || executed_pairs == 0)
			return;
		if (!pair_bytes || (start_pc & 7u) != 0 || start_pc > VU1_PROGMASK ||
			executed_pairs > (VU1_PROGSIZE - start_pc) / 8)
		{
			s_active.malformed_block_reports++;
			s_active.all_pairplan = false;
			s_active.all_fixed_shader_body = false;
			s_active.all_observation_free = false;
			Reject(UniversalPreflightRejection::InvalidPairPlan);
			return;
		}

		s_active.executed_blocks++;
		s_active.executed_pairs += executed_pairs;
		s_active.interpreter_pairs += interpreter ? executed_pairs : 0u;
		for (u32 index = 0; index < executed_pairs; index++)
		{
			const u32 pc = start_pc + index * 8;
			const u32 lower = ReadWord(pair_bytes + index * 8);
			const u32 upper = ReadWord(pair_bytes + index * 8 + 4);
			s_active.seen_pairs[(pc / 8) / 64] |=
				1ull << ((pc / 8) & 63);
			VitaVU::GpuPairPlan plan;
			const bool assume_scheduled =
				(s_active.configuration_bits &
					VitaGpuVu::UniversalConfigurationAssumeScheduled) != 0;
			const bool instant_qp =
				(s_active.configuration_bits &
					VitaGpuVu::UniversalConfigurationInstantQp) != 0;
			if (!VitaVU::AnalyzeGpuVu1PairForConfiguration(
					pc, upper, lower, assume_scheduled, instant_qp, &plan))
			{
				s_active.invalid_pairplan_pairs++;
				s_active.all_pairplan = false;
				s_active.all_fixed_shader_body = false;
				s_active.all_observation_free = false;
				Reject(UniversalPreflightRejection::InvalidPairPlan);
				continue;
			}

			VitaGpuVu::UniversalFixedPairSupport support =
				VitaGpuVu::UniversalFixedPairSupport::InvalidMetadata;
			const VitaGpuVu::UniversalPairMicroOp* encoded = nullptr;
			if (s_active.program)
			{
				encoded = &s_active.program->pairs[pc / 8];
				support = VitaGpuVu::ClassifyUniversalFixedPairSupport(
					*encoded, s_active.configuration_bits);
				RejectPairSupport(support);
			}
			else
			{
				Reject(UniversalPreflightRejection::ProgramEncoding);
			}

			const VitaGpuVu::UniversalFixedPipelineState pipeline_before =
				s_active.pipeline;
			if (encoded &&
				support != VitaGpuVu::UniversalFixedPairSupport::InvalidMetadata &&
				support != VitaGpuVu::UniversalFixedPairSupport::UnsupportedConfiguration &&
				support != VitaGpuVu::UniversalFixedPairSupport::UnsupportedNumericConfiguration)
			{
				if (!VitaGpuVu::AdvanceUniversalFixedPipeline(
						*encoded, s_active.configuration_bits,
						&s_active.pipeline, nullptr))
				{
					Reject(UniversalPreflightRejection::PipelineState);
				}
				else
				{
					constexpr u32 FlagReadMask =
						(1u << REG_STATUS_FLAG) | (1u << REG_MAC_FLAG) |
						(1u << REG_CLIP_FLAG);
					if (((encoded->upper_vi_read | encoded->lower_vi_read) &
							FlagReadMask) != 0)
					{
						for (u32 queued = 0;
							queued < pipeline_before.fmac_count; queued++)
						{
							if (pipeline_before.fmac[queued].ready_cycle >
								s_active.pipeline.cycle)
							{
								Reject(UniversalPreflightRejection::DelayedFmacFlagRead);
								break;
							}
						}
					}
				}
			}

			// The current GXP retires one pending XGKICK at the beginning of the
			// following complete pair. A pending packet at terminal E-bit is not
			// representable by its one-Execute command subset.
			s_active.pending_xgkick = false;
			if (plan.exec_lower &&
				static_cast<LowerKind>(plan.lower_kind) == LowerKind::XGKICK)
			{
				s_active.pending_xgkick = true;
			}

			const bool enabled_d = plan.dflag && (s_active.fbrst & 0x400u) != 0;
			const bool enabled_t = plan.tflag && (s_active.fbrst & 0x800u) != 0;
			if (enabled_d)
				Reject(UniversalPreflightRejection::EnabledDbit);
			if (enabled_t)
				Reject(UniversalPreflightRejection::EnabledTbit);
			RecordPlan(plan, support);
		}
	}

	void EndVuExecute(u64 cpu_us, u64 vu_cycles, u32 path1_bytes,
		bool program_active)
	{
		if (!s_enabled || !s_active.active)
			return;

		if (program_active)
			Reject(UniversalPreflightRejection::IncompleteExecution);
		if (s_active.resume)
			Reject(UniversalPreflightRejection::ResumeExecution);
		if (s_active.executed_pairs == 0)
			Reject(UniversalPreflightRejection::EmptyExecution);
		if (!s_active.all_pairplan || s_active.malformed_block_reports != 0)
			Reject(UniversalPreflightRejection::InvalidPairPlan);
		if (s_active.executed_pairs >
			VitaGpuVu::UniversalCommandEpochMaximumPairsPerExecute)
		{
			Reject(UniversalPreflightRejection::PairCapacity);
		}
		if (s_active.vif.unpack_commands + 2u >
			VitaGpuVu::UniversalCommandEpochMaximumCommands)
		{
			Reject(UniversalPreflightRejection::CommandCapacity);
		}
		if (s_active.vif.unpack_payload_bytes > 4096u)
			Reject(UniversalPreflightRejection::PayloadCapacity);
		if (s_active.vif.fixed_shader_unpack_commands !=
			s_active.vif.unpack_commands)
		{
			Reject(UniversalPreflightRejection::UnsupportedVifUnpack);
		}
		if (s_active.ebit_pairs == 0)
			Reject(UniversalPreflightRejection::MissingTerminalEbit);
		if (s_active.xgkick_pairs >
			VitaGpuVu::UniversalRawPath1ExportMaximumPackets)
		{
			Reject(UniversalPreflightRejection::Path1PacketCapacity);
		}
		if (path1_bytes > VitaGpuVu::UniversalRawPath1ExportDataQwords * 16u)
			Reject(UniversalPreflightRejection::Path1DataCapacity);
		if (s_active.pending_xgkick)
			Reject(UniversalPreflightRejection::TerminalDelayedXgkick);

		const bool preflight_candidate =
			s_active.preflight_rejection_mask == 0;

		u64 distinct_pairs = 0;
		const u64 slice_hash = ExecutedSliceHash(s_active, &distinct_pairs);
		std::lock_guard lock(s_mutex);
		s_statistics.execute_jobs++;
		s_statistics.explicit_jobs += s_active.resume ? 0u : 1u;
		s_statistics.resume_jobs += s_active.resume ? 1u : 0u;
		s_statistics.completed_jobs += program_active ? 0u : 1u;
		s_statistics.still_active_jobs += program_active ? 1u : 0u;
		s_statistics.cpu_us += cpu_us;
		s_statistics.vu_cycles += vu_cycles;
		s_statistics.executed_blocks += s_active.executed_blocks;
		s_statistics.executed_pairs += s_active.executed_pairs;
		s_statistics.pairplan_pairs += s_active.pairplan_pairs;
		s_statistics.invalid_pairplan_pairs +=
			s_active.invalid_pairplan_pairs;
		s_statistics.fixed_shader_body_pairs +=
			s_active.fixed_shader_body_pairs;
		s_statistics.observation_free_pairs +=
			s_active.observation_free_pairs;
		s_statistics.fully_fixed_body_jobs +=
			s_active.executed_pairs != 0 && s_active.all_pairplan &&
				s_active.all_fixed_shader_body ? 1u : 0u;
		s_statistics.fully_observation_free_jobs +=
			s_active.executed_pairs != 0 && s_active.all_pairplan &&
				s_active.all_observation_free ? 1u : 0u;
		s_statistics.interpreter_pairs += s_active.interpreter_pairs;
		s_statistics.malformed_block_reports +=
			s_active.malformed_block_reports;
		s_statistics.branch_pairs += s_active.branch_pairs;
		s_statistics.indirect_branch_pairs += s_active.indirect_branch_pairs;
		s_statistics.xgkick_pairs += s_active.xgkick_pairs;
		s_statistics.ebit_pairs += s_active.ebit_pairs;
		s_statistics.mbit_pairs += s_active.mbit_pairs;
		s_statistics.enabled_dbit_pairs += s_active.enabled_dbit_pairs;
		s_statistics.enabled_tbit_pairs += s_active.enabled_tbit_pairs;
		s_statistics.clip_pairs += s_active.clip_pairs;
		s_statistics.fdiv_pairs += s_active.fdiv_pairs;
		s_statistics.efu_pairs += s_active.efu_pairs;
		for (size_t kind = 0; kind < PairKindCount; kind++)
		{
			s_statistics.upper_kind_pairs[kind] +=
				s_active.upper_kind_pairs[kind];
			s_statistics.lower_kind_pairs[kind] +=
				s_active.lower_kind_pairs[kind];
		}
		const size_t pair_bucket = SizeBucket(s_active.executed_pairs);
		s_statistics.job_pair_counts[pair_bucket]++;
		s_statistics.job_pair_weight[pair_bucket] += s_active.executed_pairs;
		AddPendingToStatistics(s_active.vif);
		s_statistics.path1_jobs += path1_bytes != 0 ? 1u : 0u;
		s_statistics.path1_bytes += path1_bytes;
		s_statistics.preflight_candidate_jobs +=
			preflight_candidate ? 1u : 0u;
		s_statistics.preflight_candidate_pairs +=
			preflight_candidate ? s_active.executed_pairs : 0u;
		for (size_t reason = 0;
			reason < UniversalPreflightRejectionCount; reason++)
		{
			if ((s_active.preflight_rejection_mask & (1ull << reason)) == 0)
				continue;
			s_statistics.preflight_rejection_jobs[reason]++;
			s_statistics.preflight_rejection_pair_weight[reason] +=
				s_active.executed_pairs;
		}

		if (ProgramStatistics* program = FindOrInsertProgram(
				slice_hash, s_active.start_pc, s_active.resume,
				s_active.executed_pairs))
		{
			program->jobs++;
			program->completed_jobs += program_active ? 0u : 1u;
			program->cpu_us += cpu_us;
			program->vu_cycles += vu_cycles;
			program->executed_pairs += s_active.executed_pairs;
			program->pairplan_pairs += s_active.pairplan_pairs;
			program->fixed_shader_body_pairs +=
				s_active.fixed_shader_body_pairs;
			program->observation_free_pairs +=
				s_active.observation_free_pairs;
			program->distinct_pairs =
				std::max(program->distinct_pairs, distinct_pairs);
			program->xgkick_pairs += s_active.xgkick_pairs;
			program->path1_bytes += path1_bytes;
			program->unpack_commands += s_active.vif.unpack_commands;
			program->unpack_vectors += s_active.vif.unpack_vectors;
			program->unpack_payload_bytes +=
				s_active.vif.unpack_payload_bytes;
			program->preflight_candidate_jobs +=
				preflight_candidate ? 1u : 0u;
			program->preflight_candidate_pairs +=
				preflight_candidate ? s_active.executed_pairs : 0u;
			program->preflight_rejection_mask |=
				s_active.preflight_rejection_mask;
		}
		s_active = {};
	}

	Snapshot GetSnapshot()
	{
		std::lock_guard lock(s_mutex);
		return s_statistics;
	}

	const char* UpperKindName(u32 kind)
	{
		return VitaGpuVu::GpuVuUpperInstructionName(kind);
	}

	const char* LowerKindName(u32 kind)
	{
		return VitaGpuVu::GpuVuLowerInstructionName(kind);
	}

	const char* UniversalPreflightRejectionName(
		UniversalPreflightRejection rejection)
	{
		switch (rejection)
		{
			case UniversalPreflightRejection::ProgramEncoding:
				return "program_encoding";
			case UniversalPreflightRejection::IncompleteExecution:
				return "incomplete_execution";
			case UniversalPreflightRejection::ResumeExecution:
				return "resume_execution";
			case UniversalPreflightRejection::EmptyExecution:
				return "empty_execution";
			case UniversalPreflightRejection::InvalidPairPlan:
				return "invalid_pairplan";
			case UniversalPreflightRejection::UnsupportedConfiguration:
				return "unsupported_configuration";
			case UniversalPreflightRejection::UnsupportedNumericConfiguration:
				return "unsupported_numeric_configuration";
			case UniversalPreflightRejection::InvalidPairMetadata:
				return "invalid_pair_metadata";
			case UniversalPreflightRejection::UnsupportedUpperBody:
				return "unsupported_upper_body";
			case UniversalPreflightRejection::UnsupportedLowerBody:
				return "unsupported_lower_body";
			case UniversalPreflightRejection::ApproximateQArithmetic:
				return "approximate_q_arithmetic";
			case UniversalPreflightRejection::PipelineState:
				return "pipeline_state";
			case UniversalPreflightRejection::EnabledDbit:
				return "enabled_dbit";
			case UniversalPreflightRejection::EnabledTbit:
				return "enabled_tbit";
			case UniversalPreflightRejection::PairCapacity:
				return "pair_capacity";
			case UniversalPreflightRejection::CommandCapacity:
				return "command_capacity";
			case UniversalPreflightRejection::PayloadCapacity:
				return "payload_capacity";
			case UniversalPreflightRejection::UnsupportedVifUnpack:
				return "unsupported_vif_unpack";
			case UniversalPreflightRejection::MissingTerminalEbit:
				return "missing_terminal_ebit";
			case UniversalPreflightRejection::Path1PacketCapacity:
				return "path1_packet_capacity";
			case UniversalPreflightRejection::Path1DataCapacity:
				return "path1_data_capacity";
			case UniversalPreflightRejection::TerminalDelayedXgkick:
				return "terminal_delayed_xgkick";
			case UniversalPreflightRejection::DelayedFmacFlagRead:
				return "delayed_fmac_flag_read";
			case UniversalPreflightRejection::Count:
				break;
		}
		return "invalid";
	}
} // namespace VitaGpuVuOpportunityCensus

#endif
