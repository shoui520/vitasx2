// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugTools/IpuTrace.h"

#include "IPU/IPU.h"
#include "IPU/IPU_MultiISA.h"
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
		static constexpr std::array<char, 8> TRACE_MAGIC = {'P', 'C', 'S', 'X', '2', 'I', 'P', 'U'};
		static constexpr u32 TRACE_VERSION = 1;
		static constexpr u32 TRACE_FLAG_WAITED_FOR_ELF_ENTRY = 1u << 0;
		static constexpr u64 FNV1A64_OFFSET = 14695981039346656037ull;
		static constexpr u64 FNV1A64_PRIME = 1099511628211ull;

#pragma pack(push, 1)
		struct IpuTraceFileHeader
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

		struct IpuTraceRecord
		{
			u64 ee_cycle;
			u32 event_index;
			u8 kind;
			u8 command;
			u16 flags;
			u32 input_bits;
			u32 output_bytes;
			u64 output_hash;
			u64 ctrl_state;
			u32 command_value;
			u32 reserved;
		};
#pragma pack(pop)

		static_assert(sizeof(IpuTraceFileHeader) == 48, "IPU trace header size must stay fixed.");
		static_assert(sizeof(IpuTraceRecord) == 48, "IPU trace record size must stay fixed.");

		FILE* s_trace_file = nullptr;
		IpuTraceConfig s_config;
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
				hash ^= bytes[i];
				hash *= FNV1A64_PRIME;
			}
			return hash;
		}

		u8 CommandId(u32 command_value)
		{
			return static_cast<u8>((command_value >> 28) & 0xfu);
		}

		u32 PackInputState()
		{
			return (g_BP.BP & 0xffffu) |
				((g_BP.FP & 0xffu) << 16u) |
				((g_BP.IFC & 0xffu) << 24u);
		}

		u64 PackControlState()
		{
			return static_cast<u64>(ipuRegs.ctrl._u32) |
				(static_cast<u64>(ipuRegs.cmd.BUSY) << 32u);
		}

		IpuTraceFileHeader MakeHeader()
		{
			IpuTraceFileHeader header = {};
			std::memcpy(header.magic, TRACE_MAGIC.data(), TRACE_MAGIC.size());
			header.version = TRACE_VERSION;
			header.header_size = sizeof(IpuTraceFileHeader);
			header.record_size = sizeof(IpuTraceRecord);
			header.flags = s_config.wait_for_elf_entry ? TRACE_FLAG_WAITED_FOR_ELF_ENTRY : 0;
			header.max_records = s_config.max_records;
			header.records_written = s_records_written;
			header.entry_pc = s_entry_pc;
			return header;
		}

		bool WriteHeader()
		{
			const IpuTraceFileHeader header = MakeHeader();
			return (std::fwrite(&header, sizeof(header), 1, s_trace_file) == 1);
		}

		bool WriteRecord(const IpuTraceRecord& record)
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
				SetError("Failed to write IPU trace record.");
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
			IpuTraceRecord record = {};
			record.ee_cycle = cpuRegs.cycle;
			record.event_index = static_cast<u32>(s_records_seen);
			record.kind = IpuTraceKindDomainStatus;
			record.command = 0xffu;
			record.flags = status;
			return WriteRecord(record);
		}
	} // namespace

	bool StartIpuTrace(const IpuTraceConfig& config, Error* error)
	{
		StopIpuTrace();

		if (config.output_path.empty())
		{
			Error::SetStringView(error, "IPU trace output path is empty.");
			return false;
		}

		const std::string output_directory(Path::GetDirectory(config.output_path));
		if (!output_directory.empty() && !FileSystem::EnsureDirectoryExists(output_directory.c_str(), false, error))
			return false;

		s_trace_file = FileSystem::OpenCFile(config.output_path.c_str(), "wb");
		if (!s_trace_file)
		{
			Error::SetStringFmt(error, "Failed to open IPU trace output '{}'.", config.output_path);
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
			Error::SetStringFmt(error, "Failed to write IPU trace header to '{}'.", config.output_path);
			StopIpuTrace();
			return false;
		}
		if (s_started && !WriteStatusRecord(IpuTraceStatusValid))
		{
			Error::SetStringFmt(error, "Failed to write IPU trace status record to '{}'.", config.output_path);
			StopIpuTrace();
			return false;
		}

		return true;
	}

	void StopIpuTrace()
	{
		if (!s_trace_file)
			return;

		if (std::fseek(s_trace_file, 0, SEEK_SET) == 0)
			WriteHeader();

		std::fclose(s_trace_file);
		s_trace_file = nullptr;
		s_started = false;
	}

	bool IsIpuTraceEnabled()
	{
		return s_trace_file && s_started && !s_hit_limit;
	}

	void NotifyIpuElfEntry(u32 pc)
	{
		if (!s_trace_file || s_started)
			return;

		s_entry_pc = pc;
		s_started = true;
		WriteStatusRecord(IpuTraceStatusValid);
	}

	bool RecordIpuOutputWrite(u32 command_value, const void* data, u32 byte_count)
	{
		if (!IsIpuTraceEnabled())
			return true;

		IpuTraceRecord record = {};
		record.ee_cycle = cpuRegs.cycle;
		record.event_index = static_cast<u32>(s_records_seen);
		record.kind = IpuTraceKindOutputWrite;
		record.command = CommandId(command_value);
		record.flags = static_cast<u16>(command_value & 0xffffu);
		record.input_bits = PackInputState();
		record.output_bytes = byte_count;
		record.output_hash = HashBytes(data, byte_count);
		record.ctrl_state = PackControlState();
		record.command_value = command_value;
		return WriteRecord(record);
	}

	bool RecordIpuCommandComplete(u32 command_value)
	{
		if (!IsIpuTraceEnabled())
			return true;

		IpuTraceRecord record = {};
		record.ee_cycle = cpuRegs.cycle;
		record.event_index = static_cast<u32>(s_records_seen);
		record.kind = IpuTraceKindCommandComplete;
		record.command = CommandId(command_value);
		record.flags = static_cast<u16>(command_value & 0xffffu);
		record.input_bits = PackInputState();
		record.ctrl_state = PackControlState();
		record.command_value = command_value;
		return WriteRecord(record);
	}

	u64 GetIpuTraceRecordsWritten()
	{
		return s_records_written;
	}

	bool DidIpuTraceHitLimit()
	{
		return s_hit_limit;
	}

	const std::string& GetIpuTraceError()
	{
		return s_error;
	}
} // namespace Pcsx2Trace
