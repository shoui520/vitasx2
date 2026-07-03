// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugTools/Spu2Trace.h"

#include "R5900.h"
#include "SPU2/defs.h"

#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <utility>

namespace Pcsx2Trace
{
	namespace
	{
		static constexpr std::array<char, 8> TRACE_MAGIC = {'P', 'C', 'S', 'X', '2', 'S', 'P', 'U'};
		static constexpr u32 TRACE_VERSION = 1;
		static constexpr u32 TRACE_FLAG_WAITED_FOR_ELF_ENTRY = 1u << 0;

#pragma pack(push, 1)
		struct Spu2TraceFileHeader
		{
			char magic[8];
			u32 version;
			u32 header_size;
			u32 record_size;
			u32 flags;
			u64 max_records;
			u64 records_written;
			u32 entry_pc;
			u32 reserved;
		};

		struct Spu2TraceRecord
		{
			u64 ee_cycle;
			u64 sample_index;
			u32 event_index;
			u8 kind;
			u8 status;
			u16 reserved0;
			s32 core0_l;
			s32 core0_r;
			s32 core1_l;
			s32 core1_r;
			s32 out_l;
			s32 out_r;
			u32 reserved1;
			u32 reserved2;
			u32 reserved3;
			u32 reserved4;
		};
#pragma pack(pop)

		static_assert(sizeof(Spu2TraceFileHeader) == 48, "SPU2 trace header size must stay fixed.");
		static_assert(sizeof(Spu2TraceRecord) == 64, "SPU2 trace record size must stay fixed.");

		FILE* s_trace_file = nullptr;
		Spu2TraceConfig s_config;
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

		Spu2TraceFileHeader MakeHeader()
		{
			Spu2TraceFileHeader header = {};
			std::memcpy(header.magic, TRACE_MAGIC.data(), TRACE_MAGIC.size());
			header.version = TRACE_VERSION;
			header.header_size = sizeof(Spu2TraceFileHeader);
			header.record_size = sizeof(Spu2TraceRecord);
			header.flags = s_config.wait_for_elf_entry ? TRACE_FLAG_WAITED_FOR_ELF_ENTRY : 0;
			header.max_records = s_config.max_records;
			header.records_written = s_records_written;
			header.entry_pc = s_entry_pc;
			return header;
		}

		bool WriteHeader()
		{
			const Spu2TraceFileHeader header = MakeHeader();
			return (std::fwrite(&header, sizeof(header), 1, s_trace_file) == 1);
		}

		bool WriteRecord(const Spu2TraceRecord& record)
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
				SetError("Failed to write SPU2 trace record.");
				s_hit_limit = true;
				return false;
			}

			s_records_written++;
			s_records_seen++;
			if (s_config.max_records != 0 && s_records_written >= s_config.max_records)
				s_hit_limit = true;
			return true;
		}

		bool WriteStatusRecord(u8 status)
		{
			Spu2TraceRecord record = {};
			record.ee_cycle = cpuRegs.cycle;
			record.event_index = static_cast<u32>(s_records_seen);
			record.kind = Spu2TraceKindDomainStatus;
			record.status = status;
			return WriteRecord(record);
		}
	} // namespace

	bool StartSpu2Trace(const Spu2TraceConfig& config, Error* error)
	{
		StopSpu2Trace();

		if (config.output_path.empty())
		{
			Error::SetStringView(error, "SPU2 trace output path is empty.");
			return false;
		}

		const std::string output_directory(Path::GetDirectory(config.output_path));
		if (!output_directory.empty() && !FileSystem::EnsureDirectoryExists(output_directory.c_str(), false, error))
			return false;

		s_trace_file = FileSystem::OpenCFile(config.output_path.c_str(), "wb");
		if (!s_trace_file)
		{
			Error::SetStringFmt(error, "Failed to open SPU2 trace output '{}'.", config.output_path);
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
			Error::SetStringFmt(error, "Failed to write SPU2 trace header to '{}'.", config.output_path);
			StopSpu2Trace();
			return false;
		}
		if (s_started && !WriteStatusRecord(Spu2TraceStatusValid))
		{
			Error::SetStringFmt(error, "Failed to write SPU2 trace status record to '{}'.", config.output_path);
			StopSpu2Trace();
			return false;
		}

		return true;
	}

	void StopSpu2Trace()
	{
		if (!s_trace_file)
			return;

		if (std::fseek(s_trace_file, 0, SEEK_SET) == 0)
			WriteHeader();

		std::fclose(s_trace_file);
		s_trace_file = nullptr;
		s_started = false;
	}

	bool IsSpu2TraceEnabled()
	{
		return s_trace_file && s_started && !s_hit_limit;
	}

	void NotifySpu2ElfEntry(u32 pc)
	{
		if (!s_trace_file || s_started)
			return;

		s_entry_pc = pc;
		s_started = true;
		WriteStatusRecord(Spu2TraceStatusValid);
	}

	bool RecordSpu2OutputSample(u64 sample_index, const StereoOut32& core0,
		const StereoOut32& core1, const StereoOut32& output)
	{
		if (!IsSpu2TraceEnabled())
			return true;

		Spu2TraceRecord record = {};
		record.ee_cycle = cpuRegs.cycle;
		record.sample_index = sample_index;
		record.event_index = static_cast<u32>(s_records_seen);
		record.kind = Spu2TraceKindOutputSample;
		record.status = Spu2TraceStatusValid;
		record.core0_l = core0.Left;
		record.core0_r = core0.Right;
		record.core1_l = core1.Left;
		record.core1_r = core1.Right;
		record.out_l = output.Left;
		record.out_r = output.Right;
		return WriteRecord(record);
	}

	u64 GetSpu2TraceRecordsWritten()
	{
		return s_records_written;
	}

	bool DidSpu2TraceHitLimit()
	{
		return s_hit_limit;
	}

	const std::string& GetSpu2TraceError()
	{
		return s_error;
	}
} // namespace Pcsx2Trace
