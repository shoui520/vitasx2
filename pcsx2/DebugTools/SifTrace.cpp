// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugTools/SifTrace.h"

#include "R3000A.h"
#include "R5900.h"

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
		static constexpr std::array<char, 8> TRACE_MAGIC = {'P', 'C', 'S', 'X', '2', 'S', 'I', 'F'};
		static constexpr u32 TRACE_VERSION = 1;
		static constexpr u32 TRACE_FLAG_WAITED_FOR_ELF_ENTRY = 1u << 0;
		static constexpr u64 FNV1A64_OFFSET = 14695981039346656037ull;
		static constexpr u64 FNV1A64_PRIME = 1099511628211ull;

#pragma pack(push, 1)
		struct SifTraceFileHeader
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

		struct SifTraceRecord
		{
			u64 ee_cycle;
			u32 iop_cycle;
			u32 event_index;
			u8 kind;
			u8 channel;
			u8 direction;
			u8 status;
			u32 words;
			u32 bytes;
			u32 ee_madr;
			u32 iop_madr;
			u32 ee_qwc;
			u32 iop_counter;
			u32 fifo_before;
			u32 fifo_after;
			u32 chcr;
			u32 tag;
			u64 data_hash;
			u32 tadr;
			u32 reserved;
			u32 reserved2;
		};
#pragma pack(pop)

		static_assert(sizeof(SifTraceFileHeader) == 48, "SIF trace header size must stay fixed.");
		static_assert(sizeof(SifTraceRecord) == 80, "SIF trace record size must stay fixed.");

		FILE* s_trace_file = nullptr;
		SifTraceConfig s_config;
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

		u64 HashWords(const void* data, u32 word_count)
		{
			const u8* bytes = static_cast<const u8*>(data);
			const u32 byte_count = word_count * sizeof(u32);
			u64 hash = FNV1A64_OFFSET;
			for (u32 i = 0; i < byte_count; i++)
			{
				hash ^= bytes[i];
				hash *= FNV1A64_PRIME;
			}
			return hash;
		}

		SifTraceFileHeader MakeHeader()
		{
			SifTraceFileHeader header = {};
			std::memcpy(header.magic, TRACE_MAGIC.data(), TRACE_MAGIC.size());
			header.version = TRACE_VERSION;
			header.header_size = sizeof(SifTraceFileHeader);
			header.record_size = sizeof(SifTraceRecord);
			header.flags = s_config.wait_for_elf_entry ? TRACE_FLAG_WAITED_FOR_ELF_ENTRY : 0;
			header.max_records = s_config.max_records;
			header.records_written = s_records_written;
			header.entry_pc = s_entry_pc;
			return header;
		}

		bool WriteHeader()
		{
			const SifTraceFileHeader header = MakeHeader();
			return (std::fwrite(&header, sizeof(header), 1, s_trace_file) == 1);
		}

		bool WriteRecord(const SifTraceRecord& record)
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
				SetError("Failed to write SIF trace record.");
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
			SifTraceRecord record = {};
			record.ee_cycle = cpuRegs.cycle;
			record.iop_cycle = psxRegs.cycle;
			record.event_index = static_cast<u32>(s_records_seen);
			record.kind = SifTraceKindDomainStatus;
			record.status = status;
			return WriteRecord(record);
		}
	} // namespace

	bool StartSifTrace(const SifTraceConfig& config, Error* error)
	{
		StopSifTrace();

		if (config.output_path.empty())
		{
			Error::SetStringView(error, "SIF trace output path is empty.");
			return false;
		}

		const std::string output_directory(Path::GetDirectory(config.output_path));
		if (!output_directory.empty() && !FileSystem::EnsureDirectoryExists(output_directory.c_str(), false, error))
			return false;

		s_trace_file = FileSystem::OpenCFile(config.output_path.c_str(), "wb");
		if (!s_trace_file)
		{
			Error::SetStringFmt(error, "Failed to open SIF trace output '{}'.", config.output_path);
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
			Error::SetStringFmt(error, "Failed to write SIF trace header to '{}'.", config.output_path);
			StopSifTrace();
			return false;
		}
		if (s_started && !WriteStatusRecord(SifTraceStatusValid))
		{
			Error::SetStringFmt(error, "Failed to write SIF trace status record to '{}'.", config.output_path);
			StopSifTrace();
			return false;
		}

		return true;
	}

	void StopSifTrace()
	{
		if (!s_trace_file)
			return;

		if (std::fseek(s_trace_file, 0, SEEK_SET) == 0)
			WriteHeader();

		std::fclose(s_trace_file);
		s_trace_file = nullptr;
		s_started = false;
	}

	bool IsSifTraceEnabled()
	{
		return s_trace_file && s_started && !s_hit_limit;
	}

	void NotifySifElfEntry(u32 pc)
	{
		if (!s_trace_file || s_started)
			return;

		s_entry_pc = pc;
		s_started = true;
		WriteStatusRecord(SifTraceStatusValid);
	}

	bool RecordSifTransfer(u8 kind, u8 channel, u8 direction, const void* data, u32 word_count,
		u32 ee_madr, u32 iop_madr, u32 ee_qwc, u32 iop_counter, u32 fifo_before, u32 fifo_after,
		u32 chcr, u32 tag, u32 tadr)
	{
		if (!IsSifTraceEnabled() || word_count == 0)
			return true;

		SifTraceRecord record = {};
		record.ee_cycle = cpuRegs.cycle;
		record.iop_cycle = psxRegs.cycle;
		record.event_index = static_cast<u32>(s_records_seen);
		record.kind = kind;
		record.channel = channel;
		record.direction = direction;
		record.status = SifTraceStatusValid;
		record.words = word_count;
		record.bytes = word_count * sizeof(u32);
		record.ee_madr = ee_madr;
		record.iop_madr = iop_madr;
		record.ee_qwc = ee_qwc;
		record.iop_counter = iop_counter;
		record.fifo_before = fifo_before;
		record.fifo_after = fifo_after;
		record.chcr = chcr;
		record.tag = tag;
		record.data_hash = data ? HashWords(data, word_count) : 0;
		record.tadr = tadr;
		return WriteRecord(record);
	}

	u64 GetSifTraceRecordsWritten()
	{
		return s_records_written;
	}

	bool DidSifTraceHitLimit()
	{
		return s_hit_limit;
	}

	const std::string& GetSifTraceError()
	{
		return s_error;
	}
} // namespace Pcsx2Trace
