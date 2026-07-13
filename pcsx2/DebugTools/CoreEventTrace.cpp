// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugTools/CoreEventTrace.h"

#include "DebugTools/SifTrace.h"
#include "R3000A.h"
#include "R5900.h"

#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace Pcsx2Trace
{
	namespace
	{
		static constexpr std::array<char, 8> TRACE_MAGIC = {'P', 'C', 'S', 'X',
			'2', 'E', 'V', 'T'};
		static constexpr u32 TRACE_VERSION = 1;
		static constexpr u32 TRACE_FLAG_WAITED_FOR_ELF_ENTRY = 1u << 0;
		static constexpr u32 TRACE_FLAG_GATED_ON_SIF_RECORDS = 1u << 1;
		static constexpr u32 TRACE_EXECUTION_PROVIDER_SHIFT = 8;

#pragma pack(push, 1)
		struct CoreEventTraceFileHeader
		{
			char magic[8];
			u32 version;
			u32 header_size;
			u32 record_size;
			u32 flags;
			u64 max_records;
			u64 records_written;
			u32 entry_pc;
			u32 after_sif_records;
		};

		struct CoreEventTraceRecord
		{
			u64 ee_cycle;
			u64 iop_cycle;
			u64 target_cycle;
			u32 sequence;
			u32 ee_pc;
			u32 iop_pc;
			u32 sif_event_index;
			u8 kind;
			u8 phase;
			u8 domain;
			u8 event_id;
			u32 fields[11];
		};
#pragma pack(pop)

		static_assert(sizeof(CoreEventTraceFileHeader) == 48,
			"Core event trace header size must stay fixed.");
		static_assert(sizeof(CoreEventTraceRecord) == 88,
			"Core event trace record size must stay fixed.");

		FILE* s_trace_file = nullptr;
		CoreEventTraceConfig s_config;
		u64 s_records_seen = 0;
		u64 s_records_written = 0;
		bool s_started = false;
		bool s_hit_limit = false;
		u32 s_entry_pc = 0;
		std::string s_error;

		void SetError(std::string error)
		{
			if (s_error.empty())
				s_error = std::move(error);
		}

		u32 SaturateToU32(u64 value)
		{
			return static_cast<u32>(
				std::min<u64>(value, std::numeric_limits<u32>::max()));
		}

		CoreEventTraceFileHeader MakeHeader()
		{
			CoreEventTraceFileHeader header = {};
			std::memcpy(header.magic, TRACE_MAGIC.data(), TRACE_MAGIC.size());
			header.version = TRACE_VERSION;
			header.header_size = sizeof(CoreEventTraceFileHeader);
			header.record_size = sizeof(CoreEventTraceRecord);
			header.flags =
				(s_config.wait_for_elf_entry ? TRACE_FLAG_WAITED_FOR_ELF_ENTRY : 0) |
				(s_config.after_sif_records != 0 ? TRACE_FLAG_GATED_ON_SIF_RECORDS : 0) |
				(s_config.execution_provider_mask << TRACE_EXECUTION_PROVIDER_SHIFT);
			header.max_records = s_config.max_records;
			header.records_written = s_records_written;
			header.entry_pc = s_entry_pc;
			header.after_sif_records = SaturateToU32(s_config.after_sif_records);
			return header;
		}

		bool WriteHeader()
		{
			const CoreEventTraceFileHeader header = MakeHeader();
			return (std::fwrite(&header, sizeof(header), 1, s_trace_file) == 1);
		}

		bool WriteRecord(const CoreEventTraceRecord& record)
		{
			if (!s_trace_file || !s_started || s_hit_limit)
				return true;
			if (s_records_seen < s_config.skip_records)
			{
				s_records_seen++;
				return true;
			}
			if (s_config.max_records != 0 && s_records_written >= s_config.max_records)
			{
				s_hit_limit = true;
				return true;
			}
			if (std::fwrite(&record, sizeof(record), 1, s_trace_file) != 1)
			{
				SetError("Failed to write core event trace record.");
				s_hit_limit = true;
				return false;
			}

			s_records_written++;
			s_records_seen++;
			if (s_config.max_records != 0 && s_records_written >= s_config.max_records)
				s_hit_limit = true;
			return true;
		}
	} // namespace

	bool StartCoreEventTrace(const CoreEventTraceConfig& config, Error* error)
	{
		StopCoreEventTrace();

		if (config.output_path.empty())
		{
			Error::SetStringView(error, "Core event trace output path is empty.");
			return false;
		}

		const std::string output_directory(Path::GetDirectory(config.output_path));
		if (!output_directory.empty() &&
			!FileSystem::EnsureDirectoryExists(output_directory.c_str(), false,
				error))
		{
			return false;
		}

		s_trace_file = FileSystem::OpenCFile(config.output_path.c_str(), "wb");
		if (!s_trace_file)
		{
			Error::SetStringFmt(error, "Failed to open core event trace output '{}'.",
				config.output_path);
			return false;
		}

		s_config = config;
		s_records_seen = 0;
		s_records_written = 0;
		s_started = !s_config.wait_for_elf_entry;
		s_hit_limit = false;
		s_entry_pc = s_started ? 0xbfc00000 : 0;
		s_error.clear();

		if (!WriteHeader())
		{
			Error::SetStringFmt(error,
				"Failed to write core event trace header to '{}'.",
				config.output_path);
			StopCoreEventTrace();
			return false;
		}

		return true;
	}

	void StopCoreEventTrace()
	{
		if (!s_trace_file)
			return;

		if (std::fseek(s_trace_file, 0, SEEK_SET) == 0)
			WriteHeader();

		std::fclose(s_trace_file);
		s_trace_file = nullptr;
		s_started = false;
	}

	bool IsCoreEventTraceEnabled()
	{
		return s_trace_file && s_started && !s_hit_limit;
	}

	void NotifyCoreEventElfEntry(u32 pc)
	{
		if (!s_trace_file || s_started)
			return;

		s_entry_pc = pc;
		s_started = true;
	}

	bool RecordCoreEvent(CoreEventKind kind, CoreEventPhase phase,
		CoreEventDomain domain, CoreEventId event_id,
		u64 target_cycle, const std::array<u32, 11>& fields)
	{
		if (!IsCoreEventTraceEnabled())
			return true;

		const u64 sif_records = GetSifTraceRecordsWritten();
		if (sif_records < s_config.after_sif_records)
			return true;

		CoreEventTraceRecord record = {};
		record.ee_cycle = cpuRegs.cycle;
		record.iop_cycle = psxRegs.cycle;
		record.target_cycle = target_cycle;
		record.sequence = SaturateToU32(s_records_seen);
		record.ee_pc = cpuRegs.pc;
		record.iop_pc = psxRegs.pc;
		record.sif_event_index = SaturateToU32(sif_records);
		record.kind = static_cast<u8>(kind);
		record.phase = static_cast<u8>(phase);
		record.domain = static_cast<u8>(domain);
		record.event_id = static_cast<u8>(event_id);
		std::copy(fields.begin(), fields.end(), record.fields);
		return WriteRecord(record);
	}

	bool RecordCoreEvent(CoreEventKind kind, CoreEventPhase phase,
		CoreEventDomain domain, CoreEventId event_id,
		u64 target_cycle, u32 field0, u32 field1, u32 field2,
		u32 field3, u32 field4, u32 field5, u32 field6, u32 field7,
		u32 field8, u32 field9, u32 field10)
	{
		return RecordCoreEvent(kind, phase, domain, event_id, target_cycle,
			{{field0, field1, field2, field3, field4, field5,
				field6, field7, field8, field9, field10}});
	}

	u64 GetCoreEventTraceRecordsWritten() { return s_records_written; }

	bool DidCoreEventTraceHitLimit() { return s_hit_limit; }

	const std::string& GetCoreEventTraceError() { return s_error; }
} // namespace Pcsx2Trace
