// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <array>
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
} // namespace SaveStateRaw
