// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugTools/VuTrace.h"

#include "R5900.h"
#include "VU.h"

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
		static constexpr std::array<char, 8> TRACE_MAGIC = {'P', 'C', 'S', 'X', '2', 'V', 'U', '_'};
		static constexpr u32 TRACE_VERSION = 1;
		static constexpr u32 TRACE_FLAG_WAITED_FOR_ELF_ENTRY = 1u << 0;
		static constexpr u64 FNV1A64_OFFSET = 14695981039346656037ull;
		static constexpr u64 FNV1A64_PRIME = 1099511628211ull;

#pragma pack(push, 1)
		struct VuTraceFileHeader
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

		struct VuTraceRecord
		{
			u64 ee_cycle;
			u64 vu_cycle;
			u32 event_index;
			u16 vu_pc;
			u8 unit;
			u8 kind;
			u8 status;
			u8 reserved0;
			u16 reserved1;
			u32 upper_op;
			u32 lower_op;
			u64 vf_hash;
			u16 vi[16];
			u32 acc[4];
			u32 q;
			u32 p;
			u32 i;
			u32 r;
			u32 mac_flags;
			u32 status_flags;
			u32 clip_flags;
			u32 reserved2;
			u32 reserved3;
		};
#pragma pack(pop)

		static_assert(sizeof(VuTraceFileHeader) == 48, "VU trace header size must stay fixed.");
		static_assert(sizeof(VuTraceRecord) == 128, "VU trace record size must stay fixed.");

		FILE* s_trace_file = nullptr;
		VuTraceConfig s_config;
		u64 s_instruction_records_seen = 0;
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

		u64 HashBytes(const void* data, size_t size)
		{
			const u8* bytes = static_cast<const u8*>(data);
			u64 hash = FNV1A64_OFFSET;
			for (size_t i = 0; i < size; i++)
			{
				hash ^= bytes ? bytes[i] : 0;
				hash *= FNV1A64_PRIME;
			}
			return hash;
		}

		VuTraceFileHeader MakeHeader()
		{
			VuTraceFileHeader header = {};
			std::memcpy(header.magic, TRACE_MAGIC.data(), TRACE_MAGIC.size());
			header.version = TRACE_VERSION;
			header.header_size = sizeof(VuTraceFileHeader);
			header.record_size = sizeof(VuTraceRecord);
			header.flags = s_config.wait_for_elf_entry ? TRACE_FLAG_WAITED_FOR_ELF_ENTRY : 0;
			header.max_records = s_config.max_records;
			header.records_written = s_records_written;
			header.entry_pc = s_entry_pc;
			return header;
		}

		bool WriteHeader()
		{
			const VuTraceFileHeader header = MakeHeader();
			return (std::fwrite(&header, sizeof(header), 1, s_trace_file) == 1);
		}

		bool WriteRecord(const VuTraceRecord& record)
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
				SetError("Failed to write VU trace record.");
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
			VuTraceRecord record = {};
			record.ee_cycle = cpuRegs.cycle;
			record.event_index = static_cast<u32>(s_records_seen);
			record.kind = VuTraceKindDomainStatus;
			record.status = status;
			return WriteRecord(record);
		}
	} // namespace

	bool StartVuTrace(const VuTraceConfig& config, Error* error)
	{
		StopVuTrace();

		if (config.output_path.empty())
		{
			Error::SetStringView(error, "VU trace output path is empty.");
			return false;
		}

		const std::string output_directory(Path::GetDirectory(config.output_path));
		if (!output_directory.empty() && !FileSystem::EnsureDirectoryExists(output_directory.c_str(), false, error))
			return false;

		s_trace_file = FileSystem::OpenCFile(config.output_path.c_str(), "wb");
		if (!s_trace_file)
		{
			Error::SetStringFmt(error, "Failed to open VU trace output '{}'.", config.output_path);
			return false;
		}

		s_config = config;
		s_instruction_records_seen = 0;
		s_records_seen = 0;
		s_records_written = 0;
		s_started = !s_config.wait_for_elf_entry;
		s_hit_limit = false;
		s_entry_pc = s_started ? 0xbfc00000 : 0;
		s_error.clear();

		if (!WriteHeader())
		{
			Error::SetStringFmt(error, "Failed to write VU trace header to '{}'.", config.output_path);
			StopVuTrace();
			return false;
		}
		if (s_started && !WriteStatusRecord(VuTraceStatusValid))
		{
			Error::SetStringFmt(error, "Failed to write VU trace status record to '{}'.", config.output_path);
			StopVuTrace();
			return false;
		}

		return true;
	}

	void StopVuTrace()
	{
		if (!s_trace_file)
			return;

		if (std::fseek(s_trace_file, 0, SEEK_SET) == 0)
			WriteHeader();

		std::fclose(s_trace_file);
		s_trace_file = nullptr;
		s_started = false;
	}

	bool IsVuTraceEnabled()
	{
		return s_trace_file && s_started && !s_hit_limit;
	}

	bool RecordVuPreEeInstruction(u32 pc)
	{
		(void)pc;
		if (!s_trace_file || !s_started)
			return false;
		if (s_hit_limit)
			return true;
		if (s_config.max_instruction_records != 0 &&
			s_instruction_records_seen >= s_config.max_instruction_records)
		{
			s_hit_limit = true;
			return true;
		}
		s_instruction_records_seen++;
		if (s_config.max_instruction_records != 0 &&
			s_instruction_records_seen >= s_config.max_instruction_records)
			s_hit_limit = true;
		return s_hit_limit;
	}

	void NotifyVuElfEntry(u32 pc)
	{
		if (!s_trace_file || s_started)
			return;

		s_entry_pc = pc;
		s_started = true;
		WriteStatusRecord(VuTraceStatusValid);
	}

	bool RecordVuMicroStep(u8 unit, u16 vu_pc, u32 upper_op, u32 lower_op,
		const VURegs& vu)
	{
		if (!IsVuTraceEnabled())
			return true;
		if ((s_config.unit_mask & (1u << unit)) == 0)
			return true;

		VuTraceRecord record = {};
		record.ee_cycle = cpuRegs.cycle;
		record.vu_cycle = vu.cycle;
		record.event_index = static_cast<u32>(s_records_seen);
		record.vu_pc = vu_pc;
		record.unit = unit;
		record.kind = VuTraceKindMicroStep;
		record.status = VuTraceStatusValid;
		record.upper_op = upper_op;
		record.lower_op = lower_op;
		record.vf_hash = HashBytes(vu.VF, sizeof(vu.VF));
		for (size_t i = 0; i < std::size(record.vi); i++)
			record.vi[i] = vu.VI[i].US[0];
		for (size_t i = 0; i < std::size(record.acc); i++)
			record.acc[i] = vu.ACC.UL[i];
		record.q = vu.q.UL;
		record.p = vu.p.UL;
		record.i = vu.VI[REG_I].UL;
		record.r = vu.VI[REG_R].UL;
		record.mac_flags = vu.macflag;
		record.status_flags = vu.statusflag;
		record.clip_flags = vu.clipflag;
		return WriteRecord(record);
	}

	u64 GetVuTraceRecordsWritten()
	{
		return s_records_written;
	}

	bool DidVuTraceHitLimit()
	{
		return s_hit_limit;
	}

	const std::string& GetVuTraceError()
	{
		return s_error;
	}
} // namespace Pcsx2Trace
