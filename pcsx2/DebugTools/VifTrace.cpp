// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugTools/VifTrace.h"

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
		static constexpr std::array<char, 8> TRACE_MAGIC = {'P', 'C', 'S', 'X', '2', 'V', 'I', 'F'};
		static constexpr u32 TRACE_VERSION = 1;
		static constexpr u32 TRACE_FLAG_WAITED_FOR_ELF_ENTRY = 1u << 0;
		static constexpr u64 FNV1A64_OFFSET = 14695981039346656037ull;
		static constexpr u64 FNV1A64_PRIME = 1099511628211ull;

#pragma pack(push, 1)
		struct VifTraceFileHeader
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

		struct VifTraceRecord
		{
			u64 ee_cycle;
			u32 event_index;
			u8 unit;
			u8 kind;
			u8 command;
			u8 status;
			u32 code;
			u32 stat;
			u32 cycle;
			u32 mode;
			u32 num;
			u32 mask;
			u32 tag_addr;
			u32 tag_size;
			u32 packet_size;
			u32 madr;
			u32 qwc;
			u32 source_bytes;
			u64 source_hash;
			u64 vu_mem_hash;
			u32 cl;
			u32 wl;
			u32 reserved0;
			u32 reserved1;
		};
#pragma pack(pop)

		static_assert(sizeof(VifTraceFileHeader) == 48, "VIF trace header size must stay fixed.");
		static_assert(sizeof(VifTraceRecord) == 96, "VIF trace record size must stay fixed.");

		FILE* s_trace_file = nullptr;
		VifTraceConfig s_config;
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

		u64 HashBytes(const void* data, u32 byte_count)
		{
			const u8* bytes = static_cast<const u8*>(data);
			u64 hash = FNV1A64_OFFSET;
			for (u32 i = 0; i < byte_count; i++)
			{
				hash ^= bytes ? bytes[i] : 0;
				hash *= FNV1A64_PRIME;
			}
			return hash;
		}

		VifTraceFileHeader MakeHeader()
		{
			VifTraceFileHeader header = {};
			std::memcpy(header.magic, TRACE_MAGIC.data(), TRACE_MAGIC.size());
			header.version = TRACE_VERSION;
			header.header_size = sizeof(VifTraceFileHeader);
			header.record_size = sizeof(VifTraceRecord);
			header.flags = s_config.wait_for_elf_entry ? TRACE_FLAG_WAITED_FOR_ELF_ENTRY : 0;
			header.max_records = s_config.max_records;
			header.records_written = s_records_written;
			header.entry_pc = s_entry_pc;
			return header;
		}

		bool WriteHeader()
		{
			const VifTraceFileHeader header = MakeHeader();
			return (std::fwrite(&header, sizeof(header), 1, s_trace_file) == 1);
		}

		bool WriteRecord(const VifTraceRecord& record)
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
				SetError("Failed to write VIF trace record.");
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
			VifTraceRecord record = {};
			record.ee_cycle = cpuRegs.cycle;
			record.event_index = static_cast<u32>(s_records_seen);
			record.kind = VifTraceKindDomainStatus;
			record.command = 0xffu;
			record.status = status;
			return WriteRecord(record);
		}
	} // namespace

	bool StartVifTrace(const VifTraceConfig& config, Error* error)
	{
		StopVifTrace();

		if (config.output_path.empty())
		{
			Error::SetStringView(error, "VIF trace output path is empty.");
			return false;
		}

		const std::string output_directory(Path::GetDirectory(config.output_path));
		if (!output_directory.empty() && !FileSystem::EnsureDirectoryExists(output_directory.c_str(), false, error))
			return false;

		s_trace_file = FileSystem::OpenCFile(config.output_path.c_str(), "wb");
		if (!s_trace_file)
		{
			Error::SetStringFmt(error, "Failed to open VIF trace output '{}'.", config.output_path);
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
			Error::SetStringFmt(error, "Failed to write VIF trace header to '{}'.", config.output_path);
			StopVifTrace();
			return false;
		}
		if (s_started && !WriteStatusRecord(VifTraceStatusValid))
		{
			Error::SetStringFmt(error, "Failed to write VIF trace status record to '{}'.", config.output_path);
			StopVifTrace();
			return false;
		}

		return true;
	}

	void StopVifTrace()
	{
		if (!s_trace_file)
			return;

		if (std::fseek(s_trace_file, 0, SEEK_SET) == 0)
			WriteHeader();

		std::fclose(s_trace_file);
		s_trace_file = nullptr;
		s_started = false;
	}

	bool IsVifTraceEnabled()
	{
		return s_trace_file && s_started && !s_hit_limit;
	}

	void NotifyVifElfEntry(u32 pc)
	{
		if (!s_trace_file || s_started)
			return;

		s_entry_pc = pc;
		s_started = true;
		WriteStatusRecord(VifTraceStatusValid);
	}

	bool RecordVifCommand(u8 unit, u32 code, u32 stat, u32 cycle, u32 mode,
		u32 num, u32 mask, u32 tag_addr, u32 tag_size, u32 packet_size,
		u32 madr, u32 qwc)
	{
		if (!IsVifTraceEnabled())
			return true;

		VifTraceRecord record = {};
		record.ee_cycle = cpuRegs.cycle;
		record.event_index = static_cast<u32>(s_records_seen);
		record.unit = unit;
		record.kind = VifTraceKindCommand;
		record.command = static_cast<u8>((code >> 24) & 0x7fu);
		record.status = VifTraceStatusValid;
		record.code = code;
		record.stat = stat;
		record.cycle = cycle;
		record.mode = mode;
		record.num = num;
		record.mask = mask;
		record.tag_addr = tag_addr;
		record.tag_size = tag_size;
		record.packet_size = packet_size;
		record.madr = madr;
		record.qwc = qwc;
		record.source_bytes = sizeof(code);
		record.source_hash = HashBytes(&code, sizeof(code));
		record.cl = cycle & 0xffu;
		record.wl = (cycle >> 8) & 0xffu;
		return WriteRecord(record);
	}

	bool RecordVifUnpack(u8 unit, u32 code, u32 stat, u32 cycle, u32 mode,
		u32 num, u32 mask, u32 tag_addr, u32 tag_size, u32 packet_size,
		u32 cl, u32 wl, const void* source_data, u32 source_bytes,
		const void* vu_mem, u32 vu_mem_bytes)
	{
		if (!IsVifTraceEnabled())
			return true;

		VifTraceRecord record = {};
		record.ee_cycle = cpuRegs.cycle;
		record.event_index = static_cast<u32>(s_records_seen);
		record.unit = unit;
		record.kind = VifTraceKindUnpack;
		record.command = static_cast<u8>((code >> 24) & 0x7fu);
		record.status = VifTraceStatusValid;
		record.code = code;
		record.stat = stat;
		record.cycle = cycle;
		record.mode = mode;
		record.num = num;
		record.mask = mask;
		record.tag_addr = tag_addr;
		record.tag_size = tag_size;
		record.packet_size = packet_size;
		record.source_bytes = source_bytes;
		record.source_hash = HashBytes(source_data, source_bytes);
		record.vu_mem_hash = HashBytes(vu_mem, vu_mem_bytes);
		record.cl = cl;
		record.wl = wl;
		return WriteRecord(record);
	}

	u64 GetVifTraceRecordsWritten()
	{
		return s_records_written;
	}

	bool DidVifTraceHitLimit()
	{
		return s_hit_limit;
	}

	const std::string& GetVifTraceError()
	{
		return s_error;
	}
} // namespace Pcsx2Trace
