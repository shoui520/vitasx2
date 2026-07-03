// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugTools/IopTrace.h"
#include "R3000A.h"

#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace Pcsx2Trace
{
	namespace
	{
		static constexpr std::array<char, 8> TRACE_MAGIC = {'P', 'C', 'S', 'X', '2', 'I', 'O', 'T'};
		static constexpr u32 TRACE_VERSION = 1;

		struct IopTraceFileHeader
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

		struct IopTraceRecord
		{
			u64 index;
			u64 cycle;
			u64 next_event_cycle;
			u32 pc;
			u32 opcode;
			u32 is_delay_slot;
			u32 pc_writeback;
			u32 interrupt;
			u32 reserved;
			u32 gpr[34];
			u32 cp0[32];
			u32 cp2d[32];
			u32 cp2c[32];
		};

		static_assert(sizeof(IopTraceFileHeader) == 48, "IOP trace header size must stay fixed.");
		static_assert(sizeof(IopTraceRecord) == 568, "IOP trace record size must stay fixed.");

		FILE* s_trace_file = nullptr;
		IopTraceConfig s_config;
		u64 s_records_seen = 0;
		u64 s_records_written = 0;
		bool s_hit_limit = false;
		std::string s_error;

		IopTraceFileHeader MakeHeader()
		{
			IopTraceFileHeader header = {};
			std::memcpy(header.magic, TRACE_MAGIC.data(), TRACE_MAGIC.size());
			header.version = TRACE_VERSION;
			header.header_size = sizeof(IopTraceFileHeader);
			header.record_size = sizeof(IopTraceRecord);
			header.max_records = s_config.max_records;
			header.records_written = s_records_written;
			header.entry_pc = 0xbfc00000;
			return header;
		}

		bool WriteHeader()
		{
			const IopTraceFileHeader header = MakeHeader();
			return (std::fwrite(&header, sizeof(header), 1, s_trace_file) == 1);
		}

		void SetError(std::string error)
		{
			if (s_error.empty())
				s_error = std::move(error);
		}
	} // namespace

	bool StartIopTrace(const IopTraceConfig& config, Error* error)
	{
		StopIopTrace();

		if (config.output_path.empty())
		{
			Error::SetStringView(error, "IOP trace output path is empty.");
			return false;
		}

		const std::string output_directory(Path::GetDirectory(config.output_path));
		if (!output_directory.empty() && !FileSystem::EnsureDirectoryExists(output_directory.c_str(), false, error))
			return false;

		s_trace_file = FileSystem::OpenCFile(config.output_path.c_str(), "wb");
		if (!s_trace_file)
		{
			Error::SetStringFmt(error, "Failed to open IOP trace output '{}'.", config.output_path);
			return false;
		}

		s_config = config;
		s_records_seen = 0;
		s_records_written = 0;
		s_hit_limit = false;
		s_error.clear();

		if (!WriteHeader())
		{
			Error::SetStringFmt(error, "Failed to write IOP trace header to '{}'.", config.output_path);
			StopIopTrace();
			return false;
		}

		return true;
	}

	void StopIopTrace()
	{
		if (!s_trace_file)
			return;

		if (std::fseek(s_trace_file, 0, SEEK_SET) == 0)
			WriteHeader();

		std::fclose(s_trace_file);
		s_trace_file = nullptr;
	}

	bool IsIopTraceEnabled()
	{
		return s_trace_file != nullptr;
	}

	bool RecordIopPreInstruction(u32 pc, u32 opcode)
	{
		if (!IsIopTraceEnabled())
			return false;

		if (s_records_seen < s_config.skip_records)
		{
			s_records_seen++;
			return false;
		}

		if (s_config.max_records != 0 && s_records_written >= s_config.max_records)
		{
			s_hit_limit = true;
			return true;
		}

		IopTraceRecord record = {};
		record.index = s_records_seen;
		record.cycle = psxRegs.cycle;
		record.next_event_cycle = psxRegs.iopNextEventCycle;
		record.pc = pc;
		record.opcode = opcode;
		record.is_delay_slot = iopIsDelaySlot ? 1u : 0u;
		record.pc_writeback = psxRegs.pcWriteback;
		record.interrupt = psxRegs.interrupt;

		for (u32 i = 0; i < 34; i++)
			record.gpr[i] = psxRegs.GPR.r[i];
		for (u32 i = 0; i < 32; i++)
		{
			record.cp0[i] = psxRegs.CP0.r[i];
			record.cp2d[i] = psxRegs.CP2D.r[i];
			record.cp2c[i] = psxRegs.CP2C.r[i];
		}

		if (std::fwrite(&record, sizeof(record), 1, s_trace_file) != 1)
		{
			SetError("Failed to write IOP trace record.");
			s_hit_limit = true;
			return true;
		}

		s_records_written++;
		s_records_seen++;
		if (s_config.max_records != 0 && s_records_written >= s_config.max_records)
		{
			s_hit_limit = true;
			return true;
		}

		return false;
	}

	u64 GetIopTraceRecordsWritten()
	{
		return s_records_written;
	}

	bool DidIopTraceHitLimit()
	{
		return s_hit_limit;
	}

	const std::string& GetIopTraceError()
	{
		return s_error;
	}
} // namespace Pcsx2Trace
