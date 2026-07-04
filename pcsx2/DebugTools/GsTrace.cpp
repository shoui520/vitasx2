// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugTools/GsTrace.h"

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
		static constexpr std::array<char, 8> TRACE_MAGIC = {'P', 'C', 'S', 'X', '2', 'G', 'S', '_'};
		static constexpr u32 TRACE_VERSION = 1;
		static constexpr u32 TRACE_FLAG_WAITED_FOR_ELF_ENTRY = 1u << 0;
		static constexpr u32 TRACE_FLAG_STATE_SNAPSHOTS = 1u << 1;
		static constexpr u32 TRACE_FLAG_STATE_FULL_DUMPS = 1u << 2;
		static constexpr std::array<char, 8> STATE_DUMP_MAGIC = {'P', 'C', 'S', 'X', '2', 'G', 'S', 'F'};
		static constexpr u32 STATE_DUMP_VERSION = 1;
		static constexpr u8 GS_TRACE_SOURCE_NONE = 0xff;
		static constexpr u64 FNV1A64_OFFSET = 14695981039346656037ull;
		static constexpr u64 FNV1A64_PRIME = 1099511628211ull;

		struct GsTraceFileHeader
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

		struct GsTraceRecord
		{
			u64 ee_cycle;
			u32 draw_index;
			u8 kind;
			u8 source;
			u8 reg;
			u8 ctx;
			u64 value;
			u32 dbp;
			u32 dbw_dpsm;
			u32 rrw_rrh;
			u32 ee_pc;
			u64 data_hash;
		};

		struct GsTraceStateDumpHeader
		{
			char magic[8];
			u32 version;
			u32 header_size;
			u32 record_header_size;
			u32 flags;
			u64 records_written;
			u64 payload_bytes;
			u64 reserved;
		};

		struct GsTraceStateDumpRecord
		{
			u64 ee_cycle;
			u32 trace_record_index;
			u8 section;
			u8 status;
			u8 trigger;
			u8 reserved0;
			u64 byte_offset;
			u64 byte_count;
			u32 projection_version;
			u32 reserved1;
			u64 data_hash;
		};

		static_assert(sizeof(GsTraceFileHeader) == 48, "GS trace header size must stay fixed.");
		static_assert(sizeof(GsTraceRecord) == 48, "GS trace record size must stay fixed.");
		static_assert(sizeof(GsTraceStateDumpHeader) == 48, "GS state dump header size must stay fixed.");
		static_assert(sizeof(GsTraceStateDumpRecord) == 48, "GS state dump record header size must stay fixed.");

		FILE* s_trace_file = nullptr;
		FILE* s_state_dump_file = nullptr;
		GsTraceConfig s_config;
		u64 s_instruction_records_seen = 0;
		u64 s_records_seen = 0;
		u64 s_records_written = 0;
		u64 s_state_dump_records_written = 0;
		u64 s_state_dump_payload_bytes = 0;
		u64 s_vsync_frame = 0;
		u32 s_last_ee_pc = 0;
		u32 s_last_record_index = 0;
		u64 s_last_record_cycle = 0;
		u8 s_last_record_kind = 0xff;
		bool s_last_record_written = false;
		bool s_started = false;
		bool s_hit_limit = false;
		u32 s_entry_pc = 0;
		std::string s_error;
		thread_local u8 s_source_override = GS_TRACE_SOURCE_NONE;

		void SetError(std::string error)
		{
			if (s_error.empty())
				s_error = std::move(error);
		}

		u64 Pack64(u32 low, u32 high)
		{
			return static_cast<u64>(low) | (static_cast<u64>(high) << 32);
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

		u8 PrivilegedRegisterId(u32 physical)
		{
			switch (physical & 0x13f0u)
			{
				case 0x0000: return 0x80; // PMODE
				case 0x0010: return 0x81; // SMODE1
				case 0x0020: return 0x82; // SMODE2
				case 0x0060: return 0x83; // SYNCV
				case 0x1000: return 0x84; // CSR
				case 0x1010: return 0x85; // IMR
				case 0x1040: return 0x86; // BUSDIR
				case 0x1080: return 0x87; // SIGLBLID
				default: return 0xff;
			}
		}

		GsTraceFileHeader MakeHeader()
		{
			GsTraceFileHeader header = {};
			std::memcpy(header.magic, TRACE_MAGIC.data(), TRACE_MAGIC.size());
			header.version = TRACE_VERSION;
			header.header_size = sizeof(GsTraceFileHeader);
			header.record_size = sizeof(GsTraceRecord);
			header.flags =
				(s_config.wait_for_elf_entry ? TRACE_FLAG_WAITED_FOR_ELF_ENTRY : 0) |
				(s_config.state_snapshots ? TRACE_FLAG_STATE_SNAPSHOTS : 0) |
				(s_config.state_full_dumps ? TRACE_FLAG_STATE_FULL_DUMPS : 0);
			header.max_records = s_config.max_records;
			header.records_written = s_records_written;
			header.entry_pc = s_entry_pc;
			return header;
		}

		GsTraceStateDumpHeader MakeStateDumpHeader()
		{
			GsTraceStateDumpHeader header = {};
			std::memcpy(header.magic, STATE_DUMP_MAGIC.data(), STATE_DUMP_MAGIC.size());
			header.version = STATE_DUMP_VERSION;
			header.header_size = sizeof(GsTraceStateDumpHeader);
			header.record_header_size = sizeof(GsTraceStateDumpRecord);
			header.flags = 0;
			header.records_written = s_state_dump_records_written;
			header.payload_bytes = s_state_dump_payload_bytes;
			return header;
		}

		bool WriteHeader()
		{
			const GsTraceFileHeader header = MakeHeader();
			return std::fwrite(&header, sizeof(header), 1, s_trace_file) == 1 &&
				std::fflush(s_trace_file) == 0;
		}

		bool WriteStateDumpHeader()
		{
			if (!s_state_dump_file)
				return true;
			const GsTraceStateDumpHeader header = MakeStateDumpHeader();
			return std::fwrite(&header, sizeof(header), 1, s_state_dump_file) == 1 &&
				std::fflush(s_state_dump_file) == 0;
		}

		std::string StateDumpPathForTracePath(const std::string& trace_path)
		{
			return trace_path + ".state.bin";
		}

		bool WriteRecordAtCycle(u64 ee_cycle, u8 kind, u8 source, u8 reg, u8 ctx, u64 value, u32 dbp,
			u32 dbw_dpsm, u32 rrw_rrh, u64 data_hash)
		{
			s_last_record_written = false;
			if (!s_trace_file || !s_started)
				return true;
			if (s_hit_limit)
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

			GsTraceRecord record = {};
			record.ee_cycle = ee_cycle;
			record.draw_index = static_cast<u32>(s_records_seen);
			record.kind = kind;
			record.source = source;
			record.reg = reg;
			record.ctx = ctx;
			record.value = value;
			record.dbp = dbp;
			record.dbw_dpsm = dbw_dpsm;
			record.rrw_rrh = rrw_rrh;
			record.ee_pc = s_last_ee_pc;
			record.data_hash = data_hash;
			s_last_record_index = static_cast<u32>(s_records_seen);
			s_last_record_cycle = ee_cycle;
			s_last_record_kind = kind;
			if (std::fwrite(&record, sizeof(record), 1, s_trace_file) != 1)
			{
				SetError("Failed to write GS trace record.");
				s_hit_limit = true;
				return false;
			}

			s_records_written++;
			s_records_seen++;
			s_last_record_written = true;
			std::fflush(s_trace_file);
			if (s_config.max_records != 0 && s_records_written >= s_config.max_records)
				s_hit_limit = true;
			return true;
		}

		bool WriteRecord(u8 kind, u8 source, u8 reg, u8 ctx, u64 value, u32 dbp,
			u32 dbw_dpsm, u32 rrw_rrh, u64 data_hash)
		{
			return WriteRecordAtCycle(cpuRegs.cycle, kind, source, reg, ctx,
				value, dbp, dbw_dpsm, rrw_rrh, data_hash);
		}
	} // namespace

	ScopedGsTraceSourceOverride::ScopedGsTraceSourceOverride(u8 source)
		: m_previous_source(s_source_override)
	{
		s_source_override = source;
	}

	ScopedGsTraceSourceOverride::~ScopedGsTraceSourceOverride()
	{
		s_source_override = m_previous_source;
	}

	bool StartGsTrace(const GsTraceConfig& config, Error* error)
	{
		StopGsTrace();

		if (config.output_path.empty())
		{
			Error::SetStringView(error, "GS trace output path is empty.");
			return false;
		}

		const std::string output_directory(Path::GetDirectory(config.output_path));
		if (!output_directory.empty() && !FileSystem::EnsureDirectoryExists(output_directory.c_str(), false, error))
			return false;

		s_trace_file = FileSystem::OpenCFile(config.output_path.c_str(), "wb");
		if (!s_trace_file)
		{
			Error::SetStringFmt(error, "Failed to open GS trace output '{}'.", config.output_path);
			return false;
		}

		s_config = config;
		if (s_config.state_full_dumps)
			s_config.state_snapshots = true;
		s_instruction_records_seen = 0;
		s_records_seen = 0;
		s_records_written = 0;
		s_state_dump_records_written = 0;
		s_state_dump_payload_bytes = 0;
		s_vsync_frame = 0;
		s_last_ee_pc = 0xbfc00000;
		s_last_record_index = 0;
		s_last_record_cycle = 0;
		s_last_record_kind = 0xff;
		s_last_record_written = false;
		s_started = !s_config.wait_for_elf_entry;
		s_hit_limit = false;
		s_entry_pc = s_started ? 0xbfc00000 : 0;
		s_error.clear();

		if (s_config.state_full_dumps)
		{
			const std::string state_dump_path(StateDumpPathForTracePath(s_config.output_path));
			s_state_dump_file = FileSystem::OpenCFile(state_dump_path.c_str(), "wb");
			if (!s_state_dump_file)
			{
				Error::SetStringFmt(error, "Failed to open GS state dump output '{}'.", state_dump_path);
				StopGsTrace();
				return false;
			}
			if (!WriteStateDumpHeader())
			{
				Error::SetStringFmt(error, "Failed to write GS state dump header to '{}'.", state_dump_path);
				StopGsTrace();
				return false;
			}
		}

		if (!WriteHeader())
		{
			Error::SetStringFmt(error, "Failed to write GS trace header to '{}'.", config.output_path);
			StopGsTrace();
			return false;
		}

		return true;
	}

	void StopGsTrace()
	{
		if (s_state_dump_file)
		{
			if (std::fseek(s_state_dump_file, 0, SEEK_SET) == 0)
				WriteStateDumpHeader();
			std::fclose(s_state_dump_file);
			s_state_dump_file = nullptr;
		}

		if (!s_trace_file)
			return;

		if (std::fseek(s_trace_file, 0, SEEK_SET) == 0)
			WriteHeader();

		std::fclose(s_trace_file);
		s_trace_file = nullptr;
		s_started = false;
	}

	bool IsGsTraceEnabled()
	{
		return s_trace_file && s_started;
	}

	u8 ResolveGsTraceSource(u8 fallback_source)
	{
		return s_source_override == GS_TRACE_SOURCE_NONE ? fallback_source : s_source_override;
	}

	bool RecordGsPreEeInstruction(u32 pc)
	{
		if (!s_trace_file || !s_started)
			return false;
		s_last_ee_pc = pc;
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
		{
			s_hit_limit = true;
		}
		return s_hit_limit;
	}

	void NotifyGsElfEntry(u32 pc)
	{
		if (!s_trace_file || s_started)
			return;
		s_entry_pc = pc;
		s_last_ee_pc = pc;
		s_started = true;
	}

	bool RecordGsRegisterWrite(u8 source, u8 reg, u8 ctx, u32 low, u32 high)
	{
		return WriteRecord(GsTraceKindRegisterWrite, source, reg, ctx,
			Pack64(low, high), 0, 0, 0, 0);
	}

	bool RecordGsImageTransfer(u8 source, u32 dbp, u32 dbw, u32 dpsm, u32 rrw, u32 rrh,
		const void* data, size_t size)
	{
		return WriteRecord(GsTraceKindImageTransfer, source, 0, 0, 0, dbp,
			(dbw & 0xffffu) | ((dpsm & 0xffffu) << 16),
			(rrw & 0xffffu) | ((rrh & 0xffffu) << 16), HashBytes(data, size));
	}

	bool RecordGsRawTransfer(u8 source, const void* data, size_t size)
	{
		return WriteRecord(GsTraceKindRawTransfer, source, 0, 0,
			static_cast<u64>(size / 16), 0, 0, 0, HashBytes(data, size));
	}

	bool RecordGsVSync(u8 phase, u64 ee_cycle)
	{
		const u64 frame = s_vsync_frame;
		const bool count_frame = s_trace_file && s_started && !s_hit_limit;
		const bool wrote = WriteRecordAtCycle(ee_cycle, GsTraceKindVSync, phase,
			0, 0, frame, 0, 0, 0, 0);
		if (count_frame && wrote && phase == GsTraceVSyncEnd)
			s_vsync_frame++;
		return wrote;
	}

	bool RecordGsStateHash(u8 section, u8 status, u8 trigger, u64 byte_count,
		u64 data_hash)
	{
		return WriteRecord(GsTraceKindStateHash, section, status, trigger,
			byte_count, 0, 1, 0, data_hash);
	}

	bool RecordGsStateBytes(u8 section, u8 status, u8 trigger, const void* data,
		size_t size, u64 data_hash)
	{
		if (!s_trace_file || !s_started || !s_config.state_full_dumps || !s_state_dump_file)
			return true;
		if (!s_last_record_written || s_last_record_kind != GsTraceKindStateHash)
			return true;
		if (size != 0 && !data)
		{
			SetError("GS state dump section has null data.");
			s_hit_limit = true;
			return false;
		}

		GsTraceStateDumpRecord record = {};
		record.ee_cycle = s_last_record_cycle;
		record.trace_record_index = s_last_record_index;
		record.section = section;
		record.status = status;
		record.trigger = trigger;
		record.byte_offset = 0;
		record.byte_count = static_cast<u64>(size);
		record.projection_version = 1;
		record.data_hash = data_hash;

		if (std::fwrite(&record, sizeof(record), 1, s_state_dump_file) != 1 ||
			(size != 0 && std::fwrite(data, size, 1, s_state_dump_file) != 1))
		{
			SetError("Failed to write GS state dump record.");
			s_hit_limit = true;
			return false;
		}

		s_state_dump_records_written++;
		s_state_dump_payload_bytes += static_cast<u64>(size);
		std::fflush(s_state_dump_file);
		return true;
	}

	bool RecordGsPrivilegedWrite(u32 physical, u32 low, u32 high, bool is_64_bit)
	{
		const u8 reg = PrivilegedRegisterId(physical);
		if (reg == 0xff)
			return true;
		const u8 ctx = is_64_bit ? 1 : static_cast<u8>(physical & 0xfu);
		return RecordGsRegisterWrite(GsTraceSourcePrivileged, reg, ctx, low, high);
	}

	bool IsGsStateTraceEnabled()
	{
		return s_trace_file && s_started && s_config.state_snapshots;
	}

	bool IsGsStateFullTraceEnabled()
	{
		return s_trace_file && s_started && s_config.state_full_dumps;
	}

	u64 HashGsTraceBytes(const void* data, size_t size)
	{
		return HashBytes(data, size);
	}

	u64 GetGsTraceRecordsWritten()
	{
		return s_records_written;
	}

	bool DidGsTraceHitLimit()
	{
		return s_hit_limit;
	}

	const std::string& GetGsTraceError()
	{
		return s_error;
	}
} // namespace Pcsx2Trace
