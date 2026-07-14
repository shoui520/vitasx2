// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <array>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

class ArchiveEntryList;
class Error;

namespace SaveStateRaw
{
	// Uncompressed, host-independent transport for the ArchiveEntryList produced
	// by the portable replay serializers. SaveState_DownloadState() produces the
	// host-layout .p2s payload and must never be passed here. The required marker
	// entry and payload version make that misuse fail before an envelope is made.
	//
	// Every multibyte format field is encoded explicitly as little endian. The
	// implementation never serializes a C++ struct, size_t, pointer, or padding.
	inline constexpr u32 FORMAT_VERSION = 2;
	inline constexpr u32 PORTABLE_PAYLOAD_VERSION = 1;
	inline constexpr std::string_view PORTABLE_VERSION_ENTRY_NAME =
		"PCSX2 Portable Replay Version.id";
	inline constexpr std::array<u8, 12> PORTABLE_VERSION_ENTRY_PAYLOAD = {{
		'P', 'C', 'S', 'X', '2', 'P', 'R', 'P',
		static_cast<u8>(PORTABLE_PAYLOAD_VERSION >> 0),
		static_cast<u8>(PORTABLE_PAYLOAD_VERSION >> 8),
		static_cast<u8>(PORTABLE_PAYLOAD_VERSION >> 16),
		static_cast<u8>(PORTABLE_PAYLOAD_VERSION >> 24),
	}};

	// Serializes a complete canonical portable replay ArchiveEntryList. On
	// failure, output is left unchanged and error describes the first rejected
	// invariant.
	bool Encode(const ArchiveEntryList& entries, std::vector<u8>* output,
		Error* error = nullptr);

	// Strictly validates a complete container before exposing any ArchiveEntry.
	// The returned list owns a compact copy of every decoded payload. Unknown,
	// missing, duplicate, reordered, overlapping, out-of-bounds, corrupt, or
	// trailing data causes failure.
	std::unique_ptr<ArchiveEntryList> Decode(std::span<const u8> input,
		Error* error = nullptr);

	// A strict, bounded-memory reader for the same canonical PCSX2RAW v2
	// transport accepted by Decode(). Open() validates the complete fixed
	// header/directory, exact file length, canonical entry layout, version
	// marker, and every payload CRC before returning. ReadEntry() seeks back to
	// the cached payload location and verifies that entry's CRC again while
	// filling the caller-owned destination, closing the preflight/use race.
	class FileReader final
	{
	public:
		~FileReader();

		static std::unique_ptr<FileReader> Open(const char* filename,
			Error* error = nullptr);

		size_t GetEntryCount() const;
		std::string_view GetEntryName(u32 index) const;
		u64 GetEntrySize(u32 index) const;
		bool ReadEntry(u32 index, std::span<u8> destination,
			Error* error = nullptr);

	private:
		struct Impl;
		explicit FileReader(std::unique_ptr<Impl> impl);

		std::unique_ptr<Impl> m_impl;
	};

	// Supplies one canonical payload at a time. The returned span may refer to
	// scratch (for serialized device state) or directly to stable VM memory.
	// EncodeFile() consumes it before requesting the next entry. This permits a
	// portable replay to be emitted without constructing a 42+ MiB aggregate
	// ArchiveEntryList. Any incomplete output is deleted on failure.
	using FileEntryProvider = std::function<bool(u32 index, std::string_view name,
		std::vector<u8>* scratch, std::span<const u8>* data, Error* error)>;
	bool EncodeFile(const char* filename, const FileEntryProvider& provider,
		Error* error = nullptr);
} // namespace SaveStateRaw
