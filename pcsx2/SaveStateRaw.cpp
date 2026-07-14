// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "SaveStateRaw.h"

#include "SaveState.h"

#include "common/Error.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

namespace SaveStateRaw
{
	namespace
	{
		static constexpr std::array<u8, 8> MAGIC = {
			'P', 'C', 'S', 'X', '2', 'R', 'A', 'W'};

		// Header (56 bytes): magic[8], format_version:u32,
		// header_size:u32, g_SaveVersion:u32, portable_payload_version:u32,
		// entry_count:u32, reserved:u32, directory_offset:u64,
		// payload_offset:u64, total_size:u64.
		// Directory record (64 bytes): payload_offset:u64, size:u64,
		// IEEE CRC-32:u32, name_size:u16, flags:u16, name[40]. The
		// payloads immediately follow the directory without gaps or padding.
		static constexpr u32 HEADER_SIZE = 56;
		static constexpr u32 DIRECTORY_ENTRY_SIZE = 64;
		static constexpr u32 DIRECTORY_NAME_CAPACITY = 40;
		static constexpr u64 MAX_ENTRY_SIZE = 64 * 1024 * 1024;
		static constexpr u64 MAX_CONTAINER_SIZE = 128 * 1024 * 1024;

		// The portable replay serializer owns this order. The component names after
		// the marker identify their PCSX2 owners, but their payloads are portable
		// schemas rather than SaveState_DownloadState()'s host-layout .p2s bytes.
		static constexpr std::array<std::string_view, 16> CANONICAL_ENTRY_NAMES = {{
			PORTABLE_VERSION_ENTRY_NAME,
			"PCSX2 Internal Structures.dat",
			"eeMemory.bin",
			"iopMemory.bin",
			"eeHwRegs.bin",
			"iopHwRegs.bin",
			"Scratchpad.bin",
			"vu0Memory.bin",
			"vu1Memory.bin",
			"vu0MicroMem.bin",
			"vu1MicroMem.bin",
			"SPU2.bin",
			"USB.bin",
			"PAD.bin",
			"GS.bin",
			"Achievements.bin",
		}};
		// Mirrors BaseSavestateEntry::IsRequired() in SaveState.cpp. Optional
		// entries remain present in this transport, but may have empty payloads.
		static constexpr std::array<bool, CANONICAL_ENTRY_NAMES.size()> ENTRY_MAY_BE_EMPTY = {{
			false, false, false, false, false, false, false, false,
			false, false, false, false, false, false, false, true,
		}};

		static constexpr u64 DIRECTORY_SIZE =
			static_cast<u64>(CANONICAL_ENTRY_NAMES.size()) * DIRECTORY_ENTRY_SIZE;
		static constexpr u64 PAYLOAD_OFFSET = HEADER_SIZE + DIRECTORY_SIZE;

		static_assert(PAYLOAD_OFFSET <= MAX_CONTAINER_SIZE);
		static_assert(PORTABLE_VERSION_ENTRY_PAYLOAD.size() <= MAX_ENTRY_SIZE);
		static_assert([]() constexpr {
			for (const std::string_view name : CANONICAL_ENTRY_NAMES)
			{
				if (name.empty() || name.size() > DIRECTORY_NAME_CAPACITY)
					return false;
			}
			return true;
		}());

		constexpr std::array<u32, 256> MakeCrc32Table()
		{
			std::array<u32, 256> table = {};
			for (u32 i = 0; i < table.size(); i++)
			{
				u32 value = i;
				for (u32 bit = 0; bit < 8; bit++)
					value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0u);
				table[i] = value;
			}
			return table;
		}

		static constexpr std::array<u32, 256> CRC32_TABLE = MakeCrc32Table();

		u32 CalculateCrc32(const std::span<const u8> data)
		{
			u32 crc = 0xffffffffu;
			for (const u8 value : data)
				crc = CRC32_TABLE[(crc ^ value) & 0xffu] ^ (crc >> 8);
			return crc ^ 0xffffffffu;
		}

		bool CheckedAdd(const u64 lhs, const u64 rhs, u64* result)
		{
			if (rhs > (std::numeric_limits<u64>::max() - lhs))
				return false;

			*result = lhs + rhs;
			return true;
		}

		void StoreU16(std::vector<u8>* bytes, const size_t offset, const u16 value)
		{
			(*bytes)[offset + 0] = static_cast<u8>(value >> 0);
			(*bytes)[offset + 1] = static_cast<u8>(value >> 8);
		}

		void StoreU32(std::vector<u8>* bytes, const size_t offset, const u32 value)
		{
			for (u32 i = 0; i < 4; i++)
				(*bytes)[offset + i] = static_cast<u8>(value >> (i * 8));
		}

		void StoreU64(std::vector<u8>* bytes, const size_t offset, const u64 value)
		{
			for (u32 i = 0; i < 8; i++)
				(*bytes)[offset + i] = static_cast<u8>(value >> (i * 8));
		}

		u16 LoadU16(const std::span<const u8> bytes, const size_t offset)
		{
			return static_cast<u16>(bytes[offset + 0]) |
				(static_cast<u16>(bytes[offset + 1]) << 8);
		}

		u32 LoadU32(const std::span<const u8> bytes, const size_t offset)
		{
			u32 value = 0;
			for (u32 i = 0; i < 4; i++)
				value |= static_cast<u32>(bytes[offset + i]) << (i * 8);
			return value;
		}

		u64 LoadU64(const std::span<const u8> bytes, const size_t offset)
		{
			u64 value = 0;
			for (u32 i = 0; i < 8; i++)
				value |= static_cast<u64>(bytes[offset + i]) << (i * 8);
			return value;
		}

		size_t FindCanonicalEntry(const std::string_view name)
		{
			for (size_t i = 0; i < CANONICAL_ENTRY_NAMES.size(); i++)
			{
				if (name == CANONICAL_ENTRY_NAMES[i])
					return i;
			}
			return CANONICAL_ENTRY_NAMES.size();
		}

		struct EncodedEntry
		{
			u64 source_offset = 0;
			u64 size = 0;
			u64 payload_offset = 0;
			u32 crc32 = 0;
		};

		struct DecodedEntry
		{
			u64 payload_offset = 0;
			u64 size = 0;
		};
	} // namespace

	bool Encode(const ArchiveEntryList& entries, std::vector<u8>* output, Error* error)
	{
		if (!output)
		{
			Error::SetStringView(error, "Raw savestate output pointer is null.");
			return false;
		}

		if (entries.GetLength() != CANONICAL_ENTRY_NAMES.size())
		{
			Error::SetStringFmt(error,
				"Raw savestate requires {} canonical entries, but the source has {}.",
				CANONICAL_ENTRY_NAMES.size(), entries.GetLength());
			return false;
		}

		const std::vector<u8>& source = entries.GetBuffer();
		const std::span<const u8> source_bytes(source);
		std::array<EncodedEntry, CANONICAL_ENTRY_NAMES.size()> encoded_entries = {};
		std::array<bool, CANONICAL_ENTRY_NAMES.size()> seen = {};
		u64 container_size = PAYLOAD_OFFSET;

		for (size_t i = 0; i < CANONICAL_ENTRY_NAMES.size(); i++)
		{
			const ArchiveEntry& entry = entries[static_cast<uint>(i)];
			const std::string_view name = entry.GetFilename();
			const size_t canonical_index = FindCanonicalEntry(name);
			if (canonical_index == CANONICAL_ENTRY_NAMES.size())
			{
				Error::SetStringFmt(error, "Raw savestate entry {} has unknown name '{}'.", i, name);
				return false;
			}
			if (seen[canonical_index])
			{
				Error::SetStringFmt(error, "Raw savestate entry name '{}' is duplicated.", name);
				return false;
			}
			seen[canonical_index] = true;
			if (canonical_index != i)
			{
				Error::SetStringFmt(error,
					"Raw savestate entry {} is '{}', expected canonical entry '{}'.", i, name,
					CANONICAL_ENTRY_NAMES[i]);
				return false;
			}

			const u64 source_offset = static_cast<u64>(entry.GetDataIndex());
			const u64 size = static_cast<u64>(entry.GetDataSize());
			if (size > MAX_ENTRY_SIZE)
			{
				Error::SetStringFmt(error, "Raw savestate entry '{}' is too large ({} bytes).", name, size);
				return false;
			}
			if (size == 0 && !ENTRY_MAY_BE_EMPTY[i])
			{
				Error::SetStringFmt(error, "Required raw savestate entry '{}' is empty.", name);
				return false;
			}

			u64 source_end = 0;
			if (!CheckedAdd(source_offset, size, &source_end) || source_end > source.size())
			{
				Error::SetStringFmt(error,
					"Raw savestate entry '{}' is outside its source buffer (offset {}, size {}, buffer {}).",
					name, source_offset, size, source.size());
				return false;
			}
			if (i == 0)
			{
				const std::span<const u8> marker = source_bytes.subspan(
					static_cast<size_t>(source_offset), static_cast<size_t>(size));
				if (marker.size() != PORTABLE_VERSION_ENTRY_PAYLOAD.size() ||
					!std::equal(marker.begin(), marker.end(), PORTABLE_VERSION_ENTRY_PAYLOAD.begin()))
				{
					Error::SetStringView(error,
						"Raw savestate portable replay version marker is invalid.");
					return false;
				}
			}

			// Source entries need not be packed, but no two named components may
			// alias storage. Zero-sized optional entries have no interval.
			if (size != 0)
			{
				for (size_t previous = 0; previous < i; previous++)
				{
					const EncodedEntry& other = encoded_entries[previous];
					if (other.size == 0)
						continue;

					const u64 other_end = other.source_offset + other.size;
					if (source_offset < other_end && other.source_offset < source_end)
					{
						Error::SetStringFmt(error,
							"Raw savestate source entries '{}' and '{}' overlap.",
							CANONICAL_ENTRY_NAMES[previous], name);
						return false;
					}
				}
			}

			u64 new_container_size = 0;
			if (!CheckedAdd(container_size, size, &new_container_size) ||
				new_container_size > MAX_CONTAINER_SIZE ||
				new_container_size > std::numeric_limits<size_t>::max())
			{
				Error::SetStringFmt(error,
					"Raw savestate exceeds the {} byte container limit while adding '{}'.",
					MAX_CONTAINER_SIZE, name);
				return false;
			}

			EncodedEntry& encoded = encoded_entries[i];
			encoded.source_offset = source_offset;
			encoded.size = size;
			encoded.payload_offset = container_size;
			encoded.crc32 = CalculateCrc32(source_bytes.subspan(
				static_cast<size_t>(source_offset), static_cast<size_t>(size)));
			container_size = new_container_size;
		}

		std::vector<u8> bytes(static_cast<size_t>(container_size), 0);
		std::copy(MAGIC.begin(), MAGIC.end(), bytes.begin());
		StoreU32(&bytes, 8, FORMAT_VERSION);
		StoreU32(&bytes, 12, HEADER_SIZE);
		StoreU32(&bytes, 16, g_SaveVersion);
		StoreU32(&bytes, 20, PORTABLE_PAYLOAD_VERSION);
		StoreU32(&bytes, 24, static_cast<u32>(CANONICAL_ENTRY_NAMES.size()));
		StoreU32(&bytes, 28, 0);
		StoreU64(&bytes, 32, HEADER_SIZE);
		StoreU64(&bytes, 40, PAYLOAD_OFFSET);
		StoreU64(&bytes, 48, container_size);

		for (size_t i = 0; i < CANONICAL_ENTRY_NAMES.size(); i++)
		{
			const size_t directory_offset = HEADER_SIZE + (i * DIRECTORY_ENTRY_SIZE);
			const EncodedEntry& entry = encoded_entries[i];
			const std::string_view name = CANONICAL_ENTRY_NAMES[i];
			StoreU64(&bytes, directory_offset + 0, entry.payload_offset);
			StoreU64(&bytes, directory_offset + 8, entry.size);
			StoreU32(&bytes, directory_offset + 16, entry.crc32);
			StoreU16(&bytes, directory_offset + 20, static_cast<u16>(name.size()));
			StoreU16(&bytes, directory_offset + 22, 0);
			std::copy(name.begin(), name.end(), bytes.begin() + directory_offset + 24);

			if (entry.size != 0)
			{
				std::copy_n(source.data() + static_cast<size_t>(entry.source_offset),
					static_cast<size_t>(entry.size),
					bytes.data() + static_cast<size_t>(entry.payload_offset));
			}
		}

		*output = std::move(bytes);
		return true;
	}

	std::unique_ptr<ArchiveEntryList> Decode(const std::span<const u8> input, Error* error)
	{
		if (input.size() < HEADER_SIZE)
		{
			Error::SetStringFmt(error,
				"Raw savestate is truncated before its {} byte header ({} bytes available).",
				HEADER_SIZE, input.size());
			return nullptr;
		}

		if (!std::equal(MAGIC.begin(), MAGIC.end(), input.begin()))
		{
			Error::SetStringView(error, "Raw savestate magic is invalid.");
			return nullptr;
		}

		const u32 format_version = LoadU32(input, 8);
		const u32 header_size = LoadU32(input, 12);
		const u32 save_version = LoadU32(input, 16);
		const u32 portable_payload_version = LoadU32(input, 20);
		const u32 entry_count = LoadU32(input, 24);
		const u32 reserved = LoadU32(input, 28);
		const u64 directory_offset = LoadU64(input, 32);
		const u64 payload_offset = LoadU64(input, 40);
		const u64 total_size = LoadU64(input, 48);

		if (format_version != FORMAT_VERSION)
		{
			Error::SetStringFmt(error, "Raw savestate format version {} is unsupported (expected {}).",
				format_version, FORMAT_VERSION);
			return nullptr;
		}
		if (header_size != HEADER_SIZE)
		{
			Error::SetStringFmt(error, "Raw savestate header size {} is invalid (expected {}).",
				header_size, HEADER_SIZE);
			return nullptr;
		}
		if (save_version != g_SaveVersion)
		{
			Error::SetStringFmt(error, "Raw savestate PCSX2 version 0x{:08x} does not match 0x{:08x}.",
				save_version, g_SaveVersion);
			return nullptr;
		}
		if (portable_payload_version != PORTABLE_PAYLOAD_VERSION)
		{
			Error::SetStringFmt(error,
				"Raw savestate portable payload version {} is unsupported (expected {}).",
				portable_payload_version, PORTABLE_PAYLOAD_VERSION);
			return nullptr;
		}
		if (reserved != 0)
		{
			Error::SetStringFmt(error, "Raw savestate reserved header field is nonzero (0x{:08x}).",
				reserved);
			return nullptr;
		}
		if (entry_count != CANONICAL_ENTRY_NAMES.size())
		{
			Error::SetStringFmt(error, "Raw savestate has {} entries, expected {}.",
				entry_count, CANONICAL_ENTRY_NAMES.size());
			return nullptr;
		}
		if (directory_offset != HEADER_SIZE)
		{
			Error::SetStringFmt(error, "Raw savestate directory offset {} is invalid (expected {}).",
				directory_offset, HEADER_SIZE);
			return nullptr;
		}
		if (payload_offset != PAYLOAD_OFFSET)
		{
			Error::SetStringFmt(error, "Raw savestate payload offset {} is invalid (expected {}).",
				payload_offset, PAYLOAD_OFFSET);
			return nullptr;
		}
		if (total_size < PAYLOAD_OFFSET)
		{
			Error::SetStringFmt(error,
				"Raw savestate size {} does not contain its {} byte header and directory.",
				total_size, PAYLOAD_OFFSET);
			return nullptr;
		}
		if (total_size > MAX_CONTAINER_SIZE || total_size > std::numeric_limits<size_t>::max())
		{
			Error::SetStringFmt(error, "Raw savestate size {} exceeds the {} byte limit.",
				total_size, MAX_CONTAINER_SIZE);
			return nullptr;
		}
		if (total_size > input.size())
		{
			Error::SetStringFmt(error, "Raw savestate is truncated (declares {} bytes, has {}).",
				total_size, input.size());
			return nullptr;
		}
		if (total_size < input.size())
		{
			Error::SetStringFmt(error, "Raw savestate has {} trailing bytes.", input.size() - total_size);
			return nullptr;
		}

		std::array<DecodedEntry, CANONICAL_ENTRY_NAMES.size()> decoded_entries = {};
		std::array<bool, CANONICAL_ENTRY_NAMES.size()> seen = {};
		u64 expected_payload_offset = PAYLOAD_OFFSET;
		u64 decoded_payload_size = 0;

		for (size_t i = 0; i < CANONICAL_ENTRY_NAMES.size(); i++)
		{
			const size_t descriptor_offset = HEADER_SIZE + (i * DIRECTORY_ENTRY_SIZE);
			const u64 entry_offset = LoadU64(input, descriptor_offset + 0);
			const u64 entry_size = LoadU64(input, descriptor_offset + 8);
			const u32 expected_crc32 = LoadU32(input, descriptor_offset + 16);
			const u16 name_size = LoadU16(input, descriptor_offset + 20);
			const u16 flags = LoadU16(input, descriptor_offset + 22);

			if (flags != 0)
			{
				Error::SetStringFmt(error, "Raw savestate entry {} has unsupported flags 0x{:04x}.", i, flags);
				return nullptr;
			}
			if (name_size == 0 || name_size > DIRECTORY_NAME_CAPACITY)
			{
				Error::SetStringFmt(error, "Raw savestate entry {} has invalid name length {}.", i, name_size);
				return nullptr;
			}

			const char* const name_data = reinterpret_cast<const char*>(input.data() + descriptor_offset + 24);
			const std::string_view name(name_data, name_size);
			for (size_t unused = name_size; unused < DIRECTORY_NAME_CAPACITY; unused++)
			{
				if (input[descriptor_offset + 24 + unused] != 0)
				{
					Error::SetStringFmt(error,
						"Raw savestate entry {} has nonzero bytes after its canonical name.", i);
					return nullptr;
				}
			}

			const size_t canonical_index = FindCanonicalEntry(name);
			if (canonical_index == CANONICAL_ENTRY_NAMES.size())
			{
				Error::SetStringFmt(error, "Raw savestate entry {} has unknown name '{}'.", i, name);
				return nullptr;
			}
			if (seen[canonical_index])
			{
				Error::SetStringFmt(error, "Raw savestate entry name '{}' is duplicated.", name);
				return nullptr;
			}
			seen[canonical_index] = true;
			if (canonical_index != i)
			{
				Error::SetStringFmt(error,
					"Raw savestate entry {} is '{}', expected canonical entry '{}'.", i, name,
					CANONICAL_ENTRY_NAMES[i]);
				return nullptr;
			}

			if (entry_size > MAX_ENTRY_SIZE)
			{
				Error::SetStringFmt(error, "Raw savestate entry '{}' is too large ({} bytes).", name, entry_size);
				return nullptr;
			}
			if (entry_size == 0 && !ENTRY_MAY_BE_EMPTY[i])
			{
				Error::SetStringFmt(error, "Required raw savestate entry '{}' is empty.", name);
				return nullptr;
			}
			if (entry_offset < PAYLOAD_OFFSET)
			{
				Error::SetStringFmt(error, "Raw savestate entry '{}' points into its directory.", name);
				return nullptr;
			}

			u64 entry_end = 0;
			if (!CheckedAdd(entry_offset, entry_size, &entry_end) || entry_end > total_size)
			{
				Error::SetStringFmt(error,
					"Raw savestate entry '{}' is out of bounds (offset {}, size {}, container {}).",
					name, entry_offset, entry_size, total_size);
				return nullptr;
			}
			if (entry_offset < expected_payload_offset)
			{
				Error::SetStringFmt(error, "Raw savestate entry '{}' overlaps a preceding payload.", name);
				return nullptr;
			}
			if (entry_offset > expected_payload_offset)
			{
				Error::SetStringFmt(error, "Raw savestate has a gap before entry '{}'.", name);
				return nullptr;
			}

			const u32 actual_crc32 = CalculateCrc32(input.subspan(
				static_cast<size_t>(entry_offset), static_cast<size_t>(entry_size)));
			if (actual_crc32 != expected_crc32)
			{
				Error::SetStringFmt(error,
					"Raw savestate entry '{}' checksum mismatch (expected 0x{:08x}, got 0x{:08x}).",
					name, expected_crc32, actual_crc32);
				return nullptr;
			}
			if (i == 0)
			{
				const std::span<const u8> marker = input.subspan(
					static_cast<size_t>(entry_offset), static_cast<size_t>(entry_size));
				if (marker.size() != PORTABLE_VERSION_ENTRY_PAYLOAD.size() ||
					!std::equal(marker.begin(), marker.end(), PORTABLE_VERSION_ENTRY_PAYLOAD.begin()))
				{
					Error::SetStringView(error,
						"Raw savestate portable replay version marker is invalid.");
					return nullptr;
				}
			}

			DecodedEntry& decoded = decoded_entries[i];
			decoded.payload_offset = entry_offset;
			decoded.size = entry_size;
			expected_payload_offset = entry_end;
			decoded_payload_size += entry_size;
		}

		if (expected_payload_offset != total_size)
		{
			Error::SetStringFmt(error, "Raw savestate has {} unowned payload bytes.",
				total_size - expected_payload_offset);
			return nullptr;
		}

		std::unique_ptr<ArchiveEntryList> entries = std::make_unique<ArchiveEntryList>();
		std::vector<u8>& destination = entries->GetBuffer();
		destination.resize(static_cast<size_t>(decoded_payload_size));
		size_t destination_offset = 0;
		for (size_t i = 0; i < CANONICAL_ENTRY_NAMES.size(); i++)
		{
			const DecodedEntry& decoded = decoded_entries[i];
			if (decoded.size != 0)
			{
				std::copy_n(input.data() + static_cast<size_t>(decoded.payload_offset),
					static_cast<size_t>(decoded.size), destination.data() + destination_offset);
			}

			entries->Add(ArchiveEntry(std::string(CANONICAL_ENTRY_NAMES[i]))
				.SetDataIndex(destination_offset)
				.SetDataSize(static_cast<size_t>(decoded.size)));
			destination_offset += static_cast<size_t>(decoded.size);
		}

		return entries;
	}
} // namespace SaveStateRaw
