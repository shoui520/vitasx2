// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugTools/MemTrace.h"

#include "GS/GS.h"
#include "IopMem.h"
#include "Memory.h"
#include "R5900.h"
#include "SPU2/defs.h"
#include "VUmicro.h"

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
		static constexpr std::array<char, 8> TRACE_MAGIC = {'P', 'C', 'S', 'X', '2', 'M', 'E', 'M'};
		static constexpr u32 TRACE_VERSION = 1;
		static constexpr u32 TRACE_FLAG_WAITED_FOR_ELF_ENTRY = 1u << 0;
		static constexpr u64 FNV1A64_OFFSET = 14695981039346656037ull;
		static constexpr u64 FNV1A64_PRIME = 1099511628211ull;

		struct MemTraceFileHeader
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

		struct MemTraceRecord
		{
			u64 ee_cycle;
			u16 region_id;
			u16 block_index;
			u32 reserved;
			u64 hash;
		};

		static_assert(sizeof(MemTraceFileHeader) == 48, "MEM trace header size must stay fixed.");
		static_assert(sizeof(MemTraceRecord) == 24, "MEM trace record size must stay fixed.");

		FILE* s_trace_file = nullptr;
		MemTraceConfig s_config;
		u64 s_instruction_records_seen = 0;
		u64 s_records_seen = 0;
		u64 s_records_written = 0;
		bool s_started = false;
		bool s_hit_limit = false;
		u32 s_entry_pc = 0;
		std::string s_error;

		u64 HashBytes(const void* data, size_t size)
		{
			const u8* bytes = static_cast<const u8*>(data);
			u64 hash = FNV1A64_OFFSET;
			for (size_t i = 0; i < size; i++)
			{
				hash ^= bytes[i];
				hash *= FNV1A64_PRIME;
			}
			return hash;
		}

		u64 SampleInterval()
		{
			return s_config.sample_interval == 0 ? MemTraceDefaultHashInterval : s_config.sample_interval;
		}

		void SetError(std::string error)
		{
			if (s_error.empty())
				s_error = std::move(error);
		}

		MemTraceFileHeader MakeHeader()
		{
			MemTraceFileHeader header = {};
			std::memcpy(header.magic, TRACE_MAGIC.data(), TRACE_MAGIC.size());
			header.version = TRACE_VERSION;
			header.header_size = sizeof(MemTraceFileHeader);
			header.record_size = sizeof(MemTraceRecord);
			header.flags = s_config.wait_for_elf_entry ? TRACE_FLAG_WAITED_FOR_ELF_ENTRY : 0;
			header.max_records = s_config.max_records;
			header.records_written = s_records_written;
			header.entry_pc = s_entry_pc;
			return header;
		}

		bool WriteHeader()
		{
			const MemTraceFileHeader header = MakeHeader();
			return (std::fwrite(&header, sizeof(header), 1, s_trace_file) == 1);
		}

		bool WriteRecord(u64 ee_cycle, u16 region_id, const void* data, size_t size,
			u32 status = MemTraceRecordValid)
		{
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
			if (!data && size != 0)
			{
				SetError("MEM trace region has no data.");
				s_hit_limit = true;
				return false;
			}

			MemTraceRecord record = {};
			record.ee_cycle = ee_cycle;
			record.region_id = region_id;
			record.block_index = 0;
			record.reserved = status;
			record.hash = HashBytes(data, size);
			if (std::fwrite(&record, sizeof(record), 1, s_trace_file) != 1)
			{
				SetError("Failed to write MEM trace record.");
				s_hit_limit = true;
				return false;
			}

			s_records_written++;
			s_records_seen++;
			if (s_config.max_records != 0 && s_records_written >= s_config.max_records)
				s_hit_limit = true;
			return true;
		}

		bool WriteUnavailableRecord(u64 ee_cycle, u16 region_id)
		{
			return WriteRecord(ee_cycle, region_id, nullptr, 0, MemTraceRecordIncomplete);
		}

		bool WriteSample()
		{
			const u64 ee_cycle = cpuRegs.cycle;
			if ((s_config.region_mask & MemTraceRegionMaskEeRam) != 0 &&
				!WriteRecord(ee_cycle, MemTraceRegionEeRam, eeMem ? eeMem->Main : nullptr, Ps2MemSize::MainRam))
			{
				return false;
			}
			if ((s_config.region_mask & MemTraceRegionMaskIopRam) != 0 &&
				!WriteRecord(ee_cycle, MemTraceRegionIopRam, iopMem ? iopMem->Main : nullptr, Ps2MemSize::IopRam))
			{
				return false;
			}
			if ((s_config.region_mask & MemTraceRegionMaskEeScratchpad) != 0 &&
				!WriteRecord(ee_cycle, MemTraceRegionEeScratchpad, eeMem ? eeMem->Scratch : nullptr, Ps2MemSize::Scratch))
			{
				return false;
			}
			if ((s_config.region_mask & MemTraceRegionMaskVu0Micro) != 0 &&
				!WriteRecord(ee_cycle, MemTraceRegionVu0Micro, VU0.Micro, VU0_PROGSIZE))
			{
				return false;
			}
			if ((s_config.region_mask & MemTraceRegionMaskVu0Data) != 0 &&
				!WriteRecord(ee_cycle, MemTraceRegionVu0Data, VU0.Mem, VU0_MEMSIZE))
			{
				return false;
			}
			if ((s_config.region_mask & MemTraceRegionMaskVu1Micro) != 0 &&
				!WriteRecord(ee_cycle, MemTraceRegionVu1Micro, VU1.Micro, VU1_PROGSIZE))
			{
				return false;
			}
			if ((s_config.region_mask & MemTraceRegionMaskVu1Data) != 0 &&
				!WriteRecord(ee_cycle, MemTraceRegionVu1Data, VU1.Mem, VU1_MEMSIZE))
			{
				return false;
			}
			if ((s_config.region_mask & MemTraceRegionMaskSpu2Ram) != 0 &&
				!WriteRecord(ee_cycle, MemTraceRegionSpu2Ram, _spu2mem, 0x200000))
			{
				return false;
			}
			if ((s_config.region_mask & MemTraceRegionMaskGsLocal) != 0)
			{
				size_t gs_local_size = 0;
				const u8* gs_local = GSTraceLocalMemoryData(&gs_local_size);
				if (gs_local)
				{
					if (!WriteRecord(ee_cycle, MemTraceRegionGsLocal, gs_local, gs_local_size))
						return false;
				}
				else if (!WriteUnavailableRecord(ee_cycle, MemTraceRegionGsLocal))
				{
					return false;
				}
			}
			return true;
		}
	} // namespace

	bool StartMemTrace(const MemTraceConfig& config, Error* error)
	{
		StopMemTrace();

		if (config.output_path.empty())
		{
			Error::SetStringView(error, "MEM trace output path is empty.");
			return false;
		}

		const std::string output_directory(Path::GetDirectory(config.output_path));
		if (!output_directory.empty() && !FileSystem::EnsureDirectoryExists(output_directory.c_str(), false, error))
			return false;

		s_trace_file = FileSystem::OpenCFile(config.output_path.c_str(), "wb");
		if (!s_trace_file)
		{
			Error::SetStringFmt(error, "Failed to open MEM trace output '{}'.", config.output_path);
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
			Error::SetStringFmt(error, "Failed to write MEM trace header to '{}'.", config.output_path);
			StopMemTrace();
			return false;
		}

		return true;
	}

	void StopMemTrace()
	{
		if (!s_trace_file)
			return;

		if (std::fseek(s_trace_file, 0, SEEK_SET) == 0)
			WriteHeader();

		std::fclose(s_trace_file);
		s_trace_file = nullptr;
		s_started = false;
	}

	bool IsMemTraceEnabled()
	{
		return (s_trace_file && s_started);
	}

	bool RecordMemPreEeInstruction(u32 pc)
	{
		(void)pc;
		if (!s_trace_file)
			return false;
		if (!s_started)
			return false;
		if (s_hit_limit)
			return true;
		if (s_config.max_instruction_records != 0 &&
			s_instruction_records_seen >= s_config.max_instruction_records)
		{
			s_hit_limit = true;
			return true;
		}

		const u64 current = s_instruction_records_seen++;
		if ((current % SampleInterval()) != 0)
		{
			if (s_config.max_instruction_records != 0 &&
				s_instruction_records_seen >= s_config.max_instruction_records)
			{
				s_hit_limit = true;
				return true;
			}
			return false;
		}
		if (!WriteSample())
			return true;
		if (s_config.max_instruction_records != 0 &&
			s_instruction_records_seen >= s_config.max_instruction_records)
		{
			s_hit_limit = true;
		}
		return s_hit_limit;
	}

	void NotifyMemElfEntry(u32 pc)
	{
		if (!s_trace_file || s_started)
			return;

		s_entry_pc = pc;
		s_started = true;
	}

	u64 GetMemTraceRecordsWritten()
	{
		return s_records_written;
	}

	bool DidMemTraceHitLimit()
	{
		return s_hit_limit;
	}

	const std::string& GetMemTraceError()
	{
		return s_error;
	}
} // namespace Pcsx2Trace
