// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugTools/EeTrace.h"
#include "R5900.h"

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
		static constexpr std::array<char, 8> TRACE_MAGIC = {'P', 'C', 'S', 'X', '2', 'E', 'E', 'T'};
		static constexpr u32 TRACE_VERSION = 1;
		static constexpr u32 TRACE_FLAG_WAITED_FOR_ELF_ENTRY = 1u << 0;

		struct EeTraceFileHeader
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

		struct EeTraceRecord
		{
			u64 index;
			u64 cycle;
			u32 pc;
			u32 opcode;
			u32 sa;
			u32 branch;
			u32 is_delay_slot;
			u32 pc_writeback;
			u32 cp0[32];
			u32 fpr[32];
			u32 fprc[32];
			u32 acc;
			u32 acc_flag;
			u32 gpr[32][4];
			u32 hi[4];
			u32 lo[4];
		};

		FILE* s_trace_file = nullptr;
		EeTraceConfig s_config;
		u64 s_records_seen = 0;
		u64 s_records_written = 0;
		bool s_started = false;
		bool s_hit_limit = false;
		u32 s_entry_pc = 0;
		std::string s_error;

		EeTraceFileHeader MakeHeader()
		{
			EeTraceFileHeader header = {};
			std::memcpy(header.magic, TRACE_MAGIC.data(), TRACE_MAGIC.size());
			header.version = TRACE_VERSION;
			header.header_size = sizeof(EeTraceFileHeader);
			header.record_size = sizeof(EeTraceRecord);
			header.flags = s_config.wait_for_elf_entry ? TRACE_FLAG_WAITED_FOR_ELF_ENTRY : 0;
			header.max_records = s_config.max_records;
			header.records_written = s_records_written;
			header.entry_pc = s_entry_pc;
			return header;
		}

		bool WriteHeader()
		{
			const EeTraceFileHeader header = MakeHeader();
			return (std::fwrite(&header, sizeof(header), 1, s_trace_file) == 1);
		}

		void CaptureGpr(u32 dest[4], const GPR_reg& src)
		{
			dest[0] = src.UL[0];
			dest[1] = src.UL[1];
			dest[2] = src.UL[2];
			dest[3] = src.UL[3];
		}

		void SetError(std::string error)
		{
			if (s_error.empty())
				s_error = std::move(error);
		}
	} // namespace

	bool StartEeTrace(const EeTraceConfig& config, Error* error)
	{
		StopEeTrace();

		if (config.output_path.empty())
		{
			Error::SetStringView(error, "Trace output path is empty.");
			return false;
		}

		const std::string output_directory(Path::GetDirectory(config.output_path));
		if (!output_directory.empty() && !FileSystem::EnsureDirectoryExists(output_directory.c_str(), false, error))
			return false;

		s_trace_file = FileSystem::OpenCFile(config.output_path.c_str(), "wb");
		if (!s_trace_file)
		{
			Error::SetStringFmt(error, "Failed to open trace output '{}'.", config.output_path);
			return false;
		}

		s_config = config;
		s_records_seen = 0;
		s_records_written = 0;
		s_started = !s_config.wait_for_elf_entry;
		s_hit_limit = false;
		s_entry_pc = 0;
		s_error.clear();

		if (!WriteHeader())
		{
			Error::SetStringFmt(error, "Failed to write trace header to '{}'.", config.output_path);
			StopEeTrace();
			return false;
		}

		return true;
	}

	void StopEeTrace()
	{
		if (!s_trace_file)
			return;

		if (std::fseek(s_trace_file, 0, SEEK_SET) == 0)
			WriteHeader();

		std::fclose(s_trace_file);
		s_trace_file = nullptr;
		s_started = false;
	}

	bool IsEeTraceEnabled()
	{
		return (s_trace_file && s_started);
	}

	bool RecordEePreInstruction(u32 pc, u32 opcode)
	{
		if (!IsEeTraceEnabled())
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

		EeTraceRecord record = {};
		record.index = s_records_seen;
		record.cycle = cpuRegs.cycle;
		record.pc = pc;
		record.opcode = opcode;
		record.sa = cpuRegs.sa;
		record.branch = static_cast<u32>(cpuRegs.branch);
		record.is_delay_slot = cpuRegs.IsDelaySlot;
		record.pc_writeback = cpuRegs.pcWriteback;

		for (u32 i = 0; i < 32; i++)
		{
			record.cp0[i] = cpuRegs.CP0.r[i];
			record.fpr[i] = fpuRegs.fpr[i].UL;
			record.fprc[i] = fpuRegs.fprc[i];
			CaptureGpr(record.gpr[i], cpuRegs.GPR.r[i]);
		}

		record.acc = fpuRegs.ACC.UL;
		record.acc_flag = fpuRegs.ACCflag;
		CaptureGpr(record.hi, cpuRegs.HI);
		CaptureGpr(record.lo, cpuRegs.LO);

		if (std::fwrite(&record, sizeof(record), 1, s_trace_file) != 1)
		{
			SetError("Failed to write EE trace record.");
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

	void NotifyEeElfEntry(u32 pc)
	{
		if (!s_trace_file || s_started)
			return;

		s_entry_pc = pc;
		s_started = true;
	}

	u64 GetEeTraceRecordsWritten()
	{
		return s_records_written;
	}

	bool DidEeTraceHitLimit()
	{
		return s_hit_limit;
	}

	const std::string& GetEeTraceError()
	{
		return s_error;
	}
} // namespace Pcsx2Trace
