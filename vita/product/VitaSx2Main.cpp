// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

// VitaSX2 product entrypoint.
//
// This is intentionally a thin host. PCSX2's VM lifecycle lives in
// pcsx2/vita/VitaVmState.cpp; the product owns only Vita paths, target policy,
// persistent status/log files, and the foreground state loop.

#ifndef VITASX2_PRODUCT_BOOT_VALIDATION
#define VITASX2_PRODUCT_BOOT_VALIDATION 0
#endif

#ifndef VITASX2_WORKLOAD_REPLAY_CHECKPOINT
#define VITASX2_WORKLOAD_REPLAY_CHECKPOINT 0
#endif

#include "CDVD/CDVD.h"
#include "CDVD/CDVDcommon.h"
#include "Config.h"
#include "Counters.h"
#include "DebugTools/EeTrace.h"
#if VITASX2_PRODUCT_BOOT_VALIDATION || VITASX2_WORKLOAD_REPLAY_CHECKPOINT
#include "DebugTools/MachineCheckpointTrace.h"
#endif
#include "Host.h"
#include "INISettingsInterface.h"
#include "Input/InputManager.h"
#include "MemoryTypes.h"
#include "R3000A.h"
#include "R5900.h"
#include "SaveState.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "SIO/Pad/Pad.h"
#include "VMManager.h"
#include "VUmicro.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/FPControl.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/Threading.h"
#include "ps2/BiosTools.h"
#include "vita/VitaCore.h"
#include "vita/VitaGsMailbox.h"
#include "vita/VitaPerformanceTelemetry.h"
#include "vita/VitaVuBlockCompiler.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

#include <Sha256.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <malloc.h>
#include <memory>
#include <new>
#include <string>
#include <string_view>

extern "C" unsigned int _newlib_heap_size_user;

namespace
{
	constexpr const char* NAME = "VitaSX2";
	constexpr const char* DATA_DIR = "ux0:data/vitasx2";
	constexpr const char* BIOS_DIR = "ux0:data/vitasx2/bios";
	constexpr const char* BIOS_FILE = "SCPH-90000 JP 230-080220.bin";
	constexpr const char* BIOS_NVM_FILE = "SCPH-90000 JP 230-080220.nvm";
	constexpr const char* BIOS_MEC_FILE = "SCPH-90000 JP 230-080220.mec";
	constexpr const char* BIOS_SHA256 =
		"9e9540f7ace6651a029942c1d29b6694a9b55a80becf40b92e2033d00da9de3b";
	constexpr const char* BIOS_NVM_SHA256 =
		"94368cb696078410c77739ff5216d1db0dd03c75efe734e7bd67e502c3adeab6";
	constexpr const char* BIOS_MEC_SHA256 =
		"fbbfc6c156266ac7e9afb37f0cc53fb78fadf2f09f580e56b2da57e6539e463b";
	constexpr u64 BIOS_BYTES = 4194304;
	constexpr u64 BIOS_NVM_BYTES = 1024;
	constexpr u64 BIOS_MEC_BYTES = 4;
	constexpr const char* DISC_DIR = "ux0:data/vitasx2/disc";
	constexpr const char* ELF_DIR = "ux0:data/vitasx2/elf";
	constexpr const char* VALIDATION_DISC_PATH =
		"ux0:data/vitasx2/disc/Wander to Kyozou (Japan).iso";
	constexpr const char* BOOT_PATH_CONFIG = "ux0:data/vitasx2/boot-path.txt";
	constexpr const char* PRODUCT_CONFIG_PATH = "ux0:data/vitasx2/vitasx2.ini";
	constexpr const char* PRODUCT_MEMORY_CARD_DIR = "ux0:data/vitasx2/memcards";
	constexpr const char* PRODUCT_LOG_PATH = "ux0:data/vitasx2/vitasx2.log";
	constexpr const char* PRODUCT_LAUNCH_REQUEST_PATH =
		"ux0:data/vitasx2/vitasx2.launch-request";
	constexpr const char* PRODUCT_LAUNCH_RECEIPT_PATH =
		"ux0:data/vitasx2/vitasx2.launch-receipt";
	constexpr const char* PRODUCT_SELF_PATH = "ux0:app/VSX200001/eboot.bin";
	constexpr const char* PRODUCT_INITIALIZED_PATH =
		"ux0:data/vitasx2/vitasx2.initialized";
	constexpr const char* PRODUCT_FAILED_PATH = "ux0:data/vitasx2/vitasx2.failed";
	constexpr const char* WORKLOAD_ACTIVE_PATH =
		"ux0:data/vitasx2/workloads/active.txt";
	constexpr const char* WORKLOAD_ROOT = "ux0:data/vitasx2/workloads";
	constexpr u32 WORKLOAD_MANIFEST_VERSION = 4;
	constexpr size_t WORKLOAD_MANIFEST_MAX_BYTES = 2048;

	constexpr const char* VALIDATION_DIR = "ux0:data/vitasx2/product-validation";
	constexpr const char* VALIDATION_BIOS_DIR =
		"ux0:data/vitasx2/product-validation/bios";
	constexpr const char* VALIDATION_MEMORY_CARD_DIR =
		"ux0:data/vitasx2/product-validation/memcards";
	constexpr const char* VALIDATION_LOG_PATH =
		"ux0:data/vitasx2/product-validation/vitasx2.log";
	constexpr const char* VALIDATION_TRACE_PATH =
		"ux0:data/vitasx2/product-validation/vitasx2.ee.bin";
	constexpr const char* VALIDATION_CHECKPOINT_PATH =
		"ux0:data/vitasx2/product-validation/vitasx2.machine-checkpoint.bin";
	constexpr const char* VALIDATION_PROGRESS_PATH =
		"ux0:data/vitasx2/product-validation/vitasx2.progress";
	constexpr const char* VALIDATION_INITIALIZED_PATH =
		"ux0:data/vitasx2/product-validation/vitasx2.initialized";
	constexpr const char* VALIDATION_DONE_PATH =
		"ux0:data/vitasx2/product-validation/vitasx2.done";
	constexpr const char* VALIDATION_FAILED_PATH =
		"ux0:data/vitasx2/product-validation/vitasx2.failed";
	constexpr const char* VALIDATION_CARD_1 =
		"ux0:data/vitasx2/product-validation/memcards/Mcd001.ps2";
	constexpr const char* VALIDATION_CARD_2 =
		"ux0:data/vitasx2/product-validation/memcards/Mcd002.ps2";

	enum class ConfiguredBootKind : u8
	{
		Bios,
		Disc,
		Elf,
	};

	// Sony PSP2 SDK target/include/sceerror.h::SCE_ERROR_ERRNO_ENOENT.
	// VitaSDK does not currently expose the errno-family constants.
	constexpr s32 PSP2_ERROR_ERRNO_ENOENT = -2147418110;
	constexpr u64 VALIDATION_EE_RECORDS = 128;
	constexpr u64 VALIDATION_CHECKPOINT_RECORDS = 1;
	constexpr u64 VALIDATION_AFTER_VSYNC_FRAMES = 1280;
	constexpr u32 VALIDATION_EXPECTED_CAPTURE_FRAME = 1303;
	constexpr u32 VALIDATION_PROGRESS_FRAME_INTERVAL = 128;
	// PCSX2's CPU-thread lifecycle requires process-lifetime base and secrets
	// layers even when this frontend supplies EmuConfig directly. Empty memory
	// layers preserve all caller defaults and make base-only getters safe.
	MemorySettingsInterface s_base_settings;
	MemorySettingsInterface s_secrets_settings;

	struct WorkloadReplaySelection
	{
		struct FileIdentity
		{
			u64 bytes = 0;
			std::string sha256;
		};

		bool enabled = false;
		std::string slug;
		std::string manifest_path;
		std::string disc_basename;
		std::string state_basename;
		std::string card1_basename;
		std::string card2_basename;
		std::string disc_path;
		std::string state_path;
		std::string card1_input_path;
		std::string card2_input_path;
		std::string private_card_dir;
		FileIdentity disc_identity;
		FileIdentity state_identity;
		FileIdentity card1_identity;
		FileIdentity card2_identity;
		InputManager::VitaPadAutoFireButton input_button =
			InputManager::VitaPadAutoFireButton::None;
		u32 input_pressed_frames = 2;
		u32 input_released_frames = 6;
		u32 terminal_vsync_frames = 0;
		std::string terminal_checkpoint_path;
		std::string terminal_done_path;
		bool disc_attestation_cached = false;
		bool card1_provisioned = false;
		bool card2_provisioned = false;
	};

	void DigestToHex(const std::array<Byte, SHA256_DIGEST_SIZE>& digest,
		char (&hex)[SHA256_DIGEST_SIZE * 2 + 1])
	{
		static constexpr char digits[] = "0123456789abcdef";
		for (size_t i = 0; i < digest.size(); i++)
		{
			hex[i * 2] = digits[digest[i] >> 4];
			hex[i * 2 + 1] = digits[digest[i] & 0xf];
		}
		hex[SHA256_DIGEST_SIZE * 2] = '\0';
	}

	bool SameDateTime(const SceDateTime& left, const SceDateTime& right)
	{
		return left.year == right.year && left.month == right.month &&
			left.day == right.day && left.hour == right.hour &&
			left.minute == right.minute && left.second == right.second &&
			left.microsecond == right.microsecond;
	}

	bool VerifyFileIdentity(const char* label, const std::string& path,
		const u64 expected_size, const char* expected_sha256, Error* error,
		SceIoStat* verified_stat = nullptr)
	{
		const SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
		if (fd < 0)
		{
			Error::SetStringFmt(error,
				"Configured {} could not be opened at '{}' (0x{:08x}).",
				label, path, static_cast<u32>(fd));
			return false;
		}

		SceIoStat opened_stat = {};
		if (sceIoGetstatByFd(fd, &opened_stat) < 0 ||
			!SCE_S_ISREG(opened_stat.st_mode) || opened_stat.st_size < 0)
		{
			sceIoClose(fd);
			Error::SetStringFmt(error,
				"Configured {} is not a readable regular file at '{}'.", label,
				path);
			return false;
		}
		if (static_cast<u64>(opened_stat.st_size) != expected_size)
		{
			sceIoClose(fd);
			Error::SetStringFmt(error,
				"Configured {} has {} bytes, expected {} at '{}'.", label,
				static_cast<u64>(opened_stat.st_size), expected_size, path);
			return false;
		}

		if (!expected_sha256)
		{
			const int close_result = sceIoClose(fd);
			if (close_result < 0)
			{
				Error::SetStringFmt(error,
					"Configured {} close failed at '{}'.", label, path);
				return false;
			}
			if (verified_stat)
				*verified_stat = opened_stat;
			return true;
		}

		constexpr size_t HASH_BUFFER_SIZE = 64 * 1024;
		std::unique_ptr<Byte[]> buffer(new (std::nothrow) Byte[HASH_BUFFER_SIZE]);
		if (!buffer)
		{
			sceIoClose(fd);
			Error::SetStringFmt(error,
				"Configured {} could not allocate its SHA-256 buffer.", label);
			return false;
		}

		CSha256 sha;
		Sha256_Init(&sha);
		u64 completed = 0;
		while (completed != expected_size)
		{
			const u64 remaining = expected_size - completed;
			const SceSize request = static_cast<SceSize>(
				std::min<u64>(remaining, HASH_BUFFER_SIZE));
			const SceSSize read = sceIoRead(fd, buffer.get(), request);
			if (read <= 0)
			{
				sceIoClose(fd);
				Error::SetStringFmt(error,
					"Configured {} stopped at {}/{} bytes (0x{:08x}).", label,
					completed, expected_size, static_cast<u32>(read));
				return false;
			}
			Sha256_Update(&sha, buffer.get(), static_cast<size_t>(read));
			completed += static_cast<u64>(read);
		}

		SceIoStat completed_stat = {};
		const int completed_stat_result = sceIoGetstatByFd(fd, &completed_stat);
		const int close_result = sceIoClose(fd);
		std::array<Byte, SHA256_DIGEST_SIZE> digest = {};
		Sha256_Final(&sha, digest.data());
		char actual_sha256[SHA256_DIGEST_SIZE * 2 + 1];
		DigestToHex(digest, actual_sha256);
		const bool stable = completed_stat_result >= 0 &&
			completed_stat.st_mode == opened_stat.st_mode &&
			completed_stat.st_size == opened_stat.st_size &&
			SameDateTime(completed_stat.st_ctime, opened_stat.st_ctime) &&
			SameDateTime(completed_stat.st_mtime, opened_stat.st_mtime);
		if (close_result < 0 || !stable ||
			std::strcmp(actual_sha256, expected_sha256) != 0)
		{
			Error::SetStringFmt(error,
				"Configured {} changed while hashing or has a SHA-256 mismatch: expected {}, got {} at '{}'.",
				label, expected_sha256, actual_sha256, path);
			return false;
		}

		Console.WriteLn("VitaSX2 file authenticated: %s bytes=%llu sha256=%s path=%s.",
			label, static_cast<unsigned long long>(expected_size), actual_sha256,
			path.c_str());
		if (verified_stat)
			*verified_stat = completed_stat;
		return true;
	}

	bool VerifyConfiguredBiosBundle(const bool exact_sidecars, Error* error)
	{
		const std::string bios_path = Path::Combine(EmuFolders::Bios, BIOS_FILE);
		const std::string nvm_path = Path::Combine(EmuFolders::Bios, BIOS_NVM_FILE);
		const std::string mec_path = Path::Combine(EmuFolders::Bios, BIOS_MEC_FILE);
		return
			// The primary image is immutable product code input. Authenticate it for
			// every boot so PCSX2's FindBiosImage() fallback can never substitute a
			// different console while the frontend receipts the requested basename.
			VerifyFileIdentity("BIOS", bios_path, BIOS_BYTES,
				BIOS_SHA256, error) &&
			// NVM is intentionally persistent during ordinary product use. A loaded
			// workload, however, must begin with the same guest-visible seed as its
			// PCSX2 capture; fail closed if either companion has changed.
			VerifyFileIdentity("BIOS NVM", nvm_path, BIOS_NVM_BYTES,
				exact_sidecars ? BIOS_NVM_SHA256 : nullptr, error) &&
			VerifyFileIdentity("BIOS MEC", mec_path, BIOS_MEC_BYTES,
				exact_sidecars ? BIOS_MEC_SHA256 : nullptr, error);
	}

	bool IsLowerHexSha256(std::string_view value)
	{
		if (value.size() != SHA256_DIGEST_SIZE * 2)
			return false;
		return std::all_of(value.begin(), value.end(), [](const char ch) {
			return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
		});
	}

	bool ParsePositiveU64(std::string_view value, u64* result)
	{
		if (value.empty())
			return false;
		u64 parsed = 0;
		for (const char ch : value)
		{
			if (ch < '0' || ch > '9')
				return false;
			const u64 digit = static_cast<u64>(ch - '0');
			if (parsed > (std::numeric_limits<u64>::max() - digit) / 10)
				return false;
			parsed = parsed * 10 + digit;
		}
		if (parsed == 0)
			return false;
		*result = parsed;
		return true;
	}

	bool PublishStatus(const char* path, std::string_view contents);

	bool ReadBoundedWorkloadText(const char* path, size_t maximum_size,
		std::string* contents, Error* error)
	{
		constexpr size_t BUFFER_SIZE = 4097;
		if (maximum_size + 1 > BUFFER_SIZE)
		{
			Error::SetString(error, "Internal workload text bound is invalid.");
			return false;
		}
		std::FILE* file = FileSystem::OpenCFile(path, "rb");
		if (!file)
		{
			Error::SetStringFmt(error, "Failed to open workload file '{}'.", path);
			return false;
		}
		std::array<char, BUFFER_SIZE> buffer = {};
		const size_t read = std::fread(buffer.data(), 1, maximum_size + 1, file);
		const bool read_okay = std::ferror(file) == 0;
		const bool close_okay = std::fclose(file) == 0;
		const bool okay = read_okay && close_okay;
		if (!okay || read > maximum_size)
		{
			Error::SetStringFmt(error,
				"Workload file '{}' is unreadable or exceeds {} bytes.", path,
				maximum_size);
			return false;
		}
		if (std::find(buffer.begin(), buffer.begin() + read, '\0') !=
			buffer.begin() + read)
		{
			Error::SetStringFmt(error, "Workload file '{}' contains a NUL byte.", path);
			return false;
		}
		contents->assign(buffer.data(), read);
		return true;
	}

	bool IsWorkloadSlug(std::string_view slug)
	{
		if (slug.empty() || slug.size() > 63)
			return false;
		const auto is_lower_alnum = [](char ch) {
			return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
		};
		if (!is_lower_alnum(slug.front()) || !is_lower_alnum(slug.back()))
			return false;
		for (const char ch : slug)
		{
			if (!is_lower_alnum(ch) && ch != '-' && ch != '_')
				return false;
		}
		return true;
	}

	bool IsWorkloadBasename(std::string_view name)
	{
		if (name.empty() || name.size() > 240 || name == "." || name == ".." ||
			name.front() == ' ' || name.back() == ' ')
		{
			return false;
		}
		for (const unsigned char ch : name)
		{
			if (ch < 0x20 || ch > 0x7e || ch == '/' || ch == '\\' || ch == ':')
				return false;
		}
		return true;
	}

	bool BuildDiscAttestationRecord(const std::string& basename,
		const WorkloadReplaySelection::FileIdentity& identity,
		const SceIoStat& stat, std::string* record, Error* error)
	{
		char buffer[1024];
		const int length = std::snprintf(buffer, sizeof(buffer),
			"[VitaSX2DiscAttestationV1]\nDiscBasename=%s\nBytes=%llu\n"
			"SHA256=%s\nCTime=%u,%u,%u,%u,%u,%u,%u\n"
			"MTime=%u,%u,%u,%u,%u,%u,%u\n",
			basename.c_str(), static_cast<unsigned long long>(identity.bytes),
			identity.sha256.c_str(), stat.st_ctime.year, stat.st_ctime.month,
			stat.st_ctime.day, stat.st_ctime.hour, stat.st_ctime.minute,
			stat.st_ctime.second, stat.st_ctime.microsecond, stat.st_mtime.year,
			stat.st_mtime.month, stat.st_mtime.day, stat.st_mtime.hour,
			stat.st_mtime.minute, stat.st_mtime.second,
			stat.st_mtime.microsecond);
		if (length <= 0 || static_cast<size_t>(length) >= sizeof(buffer))
		{
			Error::SetString(error,
				"Workload disc attestation record exceeded its fixed bound.");
			return false;
		}
		record->assign(buffer, static_cast<size_t>(length));
		return true;
	}

	bool VerifyWorkloadDiscIdentity(const std::string& basename,
		const std::string& path,
		const WorkloadReplaySelection::FileIdentity& identity,
		bool* used_cache, Error* error)
	{
		*used_cache = false;
		SceIoStat stat = {};
		if (sceIoGetstat(path.c_str(), &stat) < 0 ||
			!SCE_S_ISREG(stat.st_mode) || stat.st_size < 0 ||
			static_cast<u64>(stat.st_size) != identity.bytes)
		{
			Error::SetStringFmt(error,
				"Workload disc '{}' is not the expected regular {}-byte file.",
				path, identity.bytes);
			return false;
		}

		// The disc is an immutable product asset shared by many replay capsules.
		// Key its verified stat/content record by that asset identity, not by the
		// capsule slug (whose private cards and input schedule are independent).
		// The record still includes the current basename, size, SHA-256, ctime, and
		// mtime, so any changed or substituted remote file forces a full rehash.
		const std::string cache_dir = std::string(DATA_DIR) +
			"/attestations/discs";
		const std::string cache_path =
			cache_dir + "/" + identity.sha256 + ".v1.txt";
		std::string expected_record;
		if (!BuildDiscAttestationRecord(basename, identity, stat,
				&expected_record, error))
		{
			return false;
		}

		if (FileSystem::FileExists(cache_path.c_str()))
		{
			Error cache_error;
			std::string cached_record;
			if (ReadBoundedWorkloadText(cache_path.c_str(), 1023,
					&cached_record, &cache_error) &&
				cached_record == expected_record)
			{
				*used_cache = true;
				Console.WriteLn(
					"VitaSX2 workload disc attestation cache hit: bytes=%llu sha256=%s path=%s.",
					static_cast<unsigned long long>(identity.bytes),
					identity.sha256.c_str(), path.c_str());
				return true;
			}
			Console.Warning(
				"VitaSX2 workload disc attestation cache is stale; rehashing %s.",
				path.c_str());
		}

		SceIoStat verified_stat = {};
		if (!VerifyFileIdentity("workload disc", path, identity.bytes,
				identity.sha256.c_str(), error, &verified_stat))
		{
			return false;
		}
		if (!BuildDiscAttestationRecord(basename, identity, verified_stat,
				&expected_record, error) ||
			!FileSystem::EnsureDirectoryExists(cache_dir.c_str(), true, error) ||
			!PublishStatus(cache_path.c_str(), expected_record))
		{
			if (!error->IsValid())
			{
				Error::SetString(error,
					"Failed to publish the workload disc attestation cache.");
			}
			return false;
		}
		return true;
	}

	bool ReadOptionalWorkloadReplay(WorkloadReplaySelection* selection,
		Error* error)
	{
		*selection = {};
		if (VITASX2_PRODUCT_BOOT_VALIDATION ||
			!FileSystem::FileExists(WORKLOAD_ACTIVE_PATH))
		{
			return true;
		}

		std::string slug;
		if (!ReadBoundedWorkloadText(WORKLOAD_ACTIVE_PATH, 65, &slug, error))
			return false;
		if (slug.ends_with("\n"))
		{
			slug.pop_back();
			if (slug.ends_with("\r"))
				slug.pop_back();
		}
		// The deployment harness does not delete persistent Vita data. Replacing
		// the selector with this exact sentinel restores the ordinary boot path.
		if (slug == "none")
			return true;
		if (!IsWorkloadSlug(slug))
		{
			Error::SetString(error,
				"Workload selector must be one lowercase 1-63 byte slug with only alphanumeric, '-' or '_' characters.");
			return false;
		}

		const std::string input_dir =
			std::string(WORKLOAD_ROOT) + "/" + slug + "/input";
		const std::string manifest_path = input_dir + "/workload.ini";
		std::string manifest;
		if (!ReadBoundedWorkloadText(manifest_path.c_str(),
				WORKLOAD_MANIFEST_MAX_BYTES,
				&manifest, error))
		{
			return false;
		}
		std::string normalized_manifest;
		normalized_manifest.reserve(manifest.size());
		for (size_t i = 0; i < manifest.size(); i++)
		{
			if (manifest[i] == '\r')
			{
				if (i + 1 >= manifest.size() || manifest[i + 1] != '\n')
				{
					Error::SetString(error,
						"Workload manifest contains a non-CRLF carriage return.");
					return false;
				}
				continue;
			}
			normalized_manifest.push_back(manifest[i]);
		}
		manifest = std::move(normalized_manifest);
		if (manifest.ends_with("\n"))
			manifest.pop_back();
		std::array<std::string_view, 19> lines;
		size_t line_begin = 0;
		for (size_t i = 0; i < lines.size(); i++)
		{
			const size_t line_end = manifest.find('\n', line_begin);
			if ((i + 1 < lines.size() && line_end == std::string::npos) ||
				(i + 1 == lines.size() && line_end != std::string::npos))
			{
				Error::SetString(error,
					"Workload manifest must contain exactly nineteen ordered lines.");
				return false;
			}
			const size_t end = line_end == std::string::npos ?
				manifest.size() : line_end;
			lines[i] = std::string_view(manifest).substr(line_begin, end - line_begin);
			line_begin = end + 1;
		}
		if (lines[0] != "[Workload]")
		{
			Error::SetString(error,
				"Workload manifest must begin with [Workload].");
			return false;
		}
		constexpr std::array<std::string_view, 19> KEYS = {
			"[Workload]",
			"Version=",
			"DiscBasename=",
			"DiscBytes=",
			"DiscSHA256=",
			"PortableState=",
			"PortableStateBytes=",
			"PortableStateSHA256=",
			"MemoryCard1=",
			"MemoryCard1Bytes=",
			"MemoryCard1SHA256=",
			"MemoryCard2=",
			"MemoryCard2Bytes=",
			"MemoryCard2SHA256=",
			"InputSource=",
			"InputButton=",
			"InputPressedFrames=",
			"InputReleasedFrames=",
			"TerminalVSyncFrames=",
		};
		bool keys_valid = lines[0] == KEYS[0];
		for (size_t i = 1; keys_valid && i < KEYS.size(); i++)
			keys_valid = lines[i].starts_with(KEYS[i]);
		u64 manifest_version = 0;
		if (!keys_valid ||
			!ParsePositiveU64(lines[1].substr(KEYS[1].size()),
				&manifest_version) ||
			manifest_version != WORKLOAD_MANIFEST_VERSION)
		{
			Error::SetString(error,
				"Workload manifest keys, version, or ordering are invalid.");
			return false;
		}
		const std::string disc_basename(lines[2].substr(KEYS[2].size()));
		const std::string state_basename(lines[5].substr(KEYS[5].size()));
		const std::string card1_basename(lines[8].substr(KEYS[8].size()));
		const std::string card2_basename(lines[11].substr(KEYS[11].size()));
		if (!IsWorkloadBasename(disc_basename) ||
			!StringUtil::EndsWithNoCase(disc_basename, ".iso") ||
			!IsWorkloadBasename(state_basename) ||
			!StringUtil::EndsWithNoCase(state_basename, ".pcsx2raw") ||
			!IsWorkloadBasename(card1_basename) ||
			!StringUtil::EndsWithNoCase(card1_basename, ".ps2") ||
			!IsWorkloadBasename(card2_basename) ||
			!StringUtil::EndsWithNoCase(card2_basename, ".ps2") ||
			StringUtil::Strcasecmp(card1_basename.c_str(),
				card2_basename.c_str()) == 0)
		{
			Error::SetString(error,
				"Workload manifest must name one flat ISO, one PCSX2RAW state, and two distinct flat PS2 memory-card inputs.");
			return false;
		}
		WorkloadReplaySelection::FileIdentity disc_identity;
		WorkloadReplaySelection::FileIdentity state_identity;
		WorkloadReplaySelection::FileIdentity card1_identity;
		WorkloadReplaySelection::FileIdentity card2_identity;
		const auto parse_identity = [&](const size_t bytes_index,
			const size_t sha_index, const char* label,
			WorkloadReplaySelection::FileIdentity* identity) {
			const std::string_view bytes =
				lines[bytes_index].substr(KEYS[bytes_index].size());
			const std::string_view sha256 =
				lines[sha_index].substr(KEYS[sha_index].size());
			if (!ParsePositiveU64(bytes, &identity->bytes) ||
				!IsLowerHexSha256(sha256))
			{
				Error::SetStringFmt(error,
					"Workload manifest {} identity is malformed.", label);
				return false;
			}
			identity->sha256.assign(sha256);
			return true;
		};
		if (!parse_identity(3, 4, "disc", &disc_identity) ||
			!parse_identity(6, 7, "portable state", &state_identity) ||
			!parse_identity(9, 10, "memory card 1", &card1_identity) ||
			!parse_identity(12, 13, "memory card 2", &card2_identity))
		{
			return false;
		}
		if (lines[14] != "InputSource=DeterministicVSyncV1")
		{
			Error::SetString(error,
				"Workload manifest input source is unsupported.");
			return false;
		}
		InputManager::VitaPadAutoFireButton input_button =
			InputManager::VitaPadAutoFireButton::None;
		const std::string_view input_button_name =
			lines[15].substr(KEYS[15].size());
		if (input_button_name == "Cross")
			input_button = InputManager::VitaPadAutoFireButton::Cross;
		else if (input_button_name == "Circle")
			input_button = InputManager::VitaPadAutoFireButton::Circle;
		else if (input_button_name != "None")
		{
			Error::SetString(error,
				"Workload manifest input button is unsupported.");
			return false;
		}
		u64 input_pressed_frames = 0;
		u64 input_released_frames = 0;
		u64 terminal_vsync_frames = 0;
		if (!ParsePositiveU64(lines[16].substr(KEYS[16].size()),
				&input_pressed_frames) ||
			!ParsePositiveU64(lines[17].substr(KEYS[17].size()),
				&input_released_frames) ||
			input_pressed_frames > 600 || input_released_frames > 600 ||
			!ParsePositiveU64(lines[18].substr(KEYS[18].size()),
				&terminal_vsync_frames) ||
			terminal_vsync_frames > 36000)
		{
			Error::SetString(error,
				"Workload manifest input cadence or terminal VSync frame is malformed.");
			return false;
		}

		// The large immutable disc remains in VitaSX2's existing disc store. Only
		// the basename crosses the workload manifest, so this cannot broaden the
		// unsafe-homebrew frontend into an arbitrary device path.
		const std::string disc_path =
			std::string(DISC_DIR) + "/" + disc_basename;
		const std::string state_path = input_dir + "/" + state_basename;
		const std::string card1_input_path = input_dir + "/" + card1_basename;
		const std::string card2_input_path = input_dir + "/" + card2_basename;
		if (!FileSystem::FileExists(disc_path.c_str()) ||
			!FileSystem::FileExists(state_path.c_str()) ||
			!FileSystem::FileExists(card1_input_path.c_str()) ||
			!FileSystem::FileExists(card2_input_path.c_str()))
		{
			Error::SetStringFmt(error,
				"Workload '{}' is missing its disc, portable state, or private memory-card inputs.",
				slug);
			return false;
		}
		bool disc_attestation_cached = false;
		if (!VerifyWorkloadDiscIdentity(disc_basename, disc_path,
				disc_identity, &disc_attestation_cached, error) ||
			!VerifyFileIdentity("workload portable state", state_path,
				state_identity.bytes, state_identity.sha256.c_str(), error) ||
			!VerifyFileIdentity("workload memory card 1", card1_input_path,
				card1_identity.bytes, card1_identity.sha256.c_str(), error) ||
			!VerifyFileIdentity("workload memory card 2", card2_input_path,
				card2_identity.bytes, card2_identity.sha256.c_str(), error))
		{
			return false;
		}

		selection->enabled = true;
		selection->slug = std::move(slug);
		selection->manifest_path = manifest_path;
		selection->disc_basename = disc_basename;
		selection->state_basename = state_basename;
		selection->card1_basename = card1_basename;
		selection->card2_basename = card2_basename;
		selection->disc_path = disc_path;
		selection->state_path = state_path;
		selection->card1_input_path = card1_input_path;
		selection->card2_input_path = card2_input_path;
		selection->disc_identity = std::move(disc_identity);
		selection->state_identity = std::move(state_identity);
		selection->card1_identity = std::move(card1_identity);
		selection->card2_identity = std::move(card2_identity);
		selection->input_button = input_button;
		selection->input_pressed_frames = static_cast<u32>(input_pressed_frames);
		selection->input_released_frames = static_cast<u32>(input_released_frames);
		selection->terminal_vsync_frames = static_cast<u32>(terminal_vsync_frames);
		selection->disc_attestation_cached = disc_attestation_cached;
		selection->private_card_dir =
			std::string(WORKLOAD_ROOT) + "/" + selection->slug +
			"/runtime/memcards";
		selection->terminal_checkpoint_path =
			std::string(WORKLOAD_ROOT) + "/" + selection->slug +
			"/runtime/terminal.machine-checkpoint.bin";
		selection->terminal_done_path =
			std::string(WORKLOAD_ROOT) + "/" + selection->slug +
			"/runtime/terminal.done";
		Console.WriteLn(
			"VitaSX2 workload selector validated: slug=%s manifest=%s disc=%s state=%s card1=%s card2=%s.",
			selection->slug.c_str(), selection->manifest_path.c_str(),
			selection->disc_basename.c_str(), selection->state_basename.c_str(),
			selection->card1_basename.c_str(), selection->card2_basename.c_str());
		return true;
	}

	bool ProvisionPrivateWorkloadCard(const std::string& source,
		const std::string& destination, bool* provisioned, Error* error)
	{
		*provisioned = false;
		const auto get_supported_size = [](const std::string& path,
			u64* size, Error* local_error) {
			SceIoStat stat = {};
			const int result = sceIoGetstat(path.c_str(), &stat);
			if (result < 0 || !SCE_S_ISREG(stat.st_mode) || stat.st_size < 0 ||
				!FileMcd_IsSupportedCardFileSize(static_cast<s64>(stat.st_size)))
			{
				Error::SetStringFmt(local_error,
					"Workload memory card '{}' is not a regular PCSX2-supported card file (result=0x{:08x}, bytes={}).",
					path, static_cast<u32>(result),
					stat.st_size < 0 ? 0ull : static_cast<u64>(stat.st_size));
				return false;
			}
			*size = static_cast<u64>(stat.st_size);
			return true;
		};

		u64 source_size = 0;
		if (!get_supported_size(source, &source_size, error))
			return false;
		if (FileSystem::FileExists(destination.c_str()))
		{
			u64 destination_size = 0;
			if (!get_supported_size(destination, &destination_size, error))
				return false;
			if (destination_size != source_size)
			{
				Error::SetStringFmt(error,
					"Existing private workload memory card '{}' has {} bytes, expected {}. It will not be overwritten.",
					destination, destination_size, source_size);
				return false;
			}
			return true;
		}

		const std::string temporary = destination + ".provisioning";
		FileSystem::DeleteFilePath(temporary.c_str());
		const SceUID source_fd = sceIoOpen(source.c_str(), SCE_O_RDONLY, 0);
		if (source_fd < 0)
		{
			Error::SetStringFmt(error,
				"Failed to open workload memory-card input '{}' (0x{:08x}).",
				source, static_cast<u32>(source_fd));
			return false;
		}
		SceIoStat opened_source = {};
		if (sceIoGetstatByFd(source_fd, &opened_source) < 0 ||
			!SCE_S_ISREG(opened_source.st_mode) || opened_source.st_size < 0 ||
			static_cast<u64>(opened_source.st_size) != source_size)
		{
			sceIoClose(source_fd);
			Error::SetStringFmt(error,
				"Workload memory-card input '{}' changed before provisioning.", source);
			return false;
		}

		const SceUID destination_fd = sceIoOpen(temporary.c_str(),
			SCE_O_WRONLY | SCE_O_CREAT | SCE_O_EXCL, 0666);
		if (destination_fd < 0)
		{
			sceIoClose(source_fd);
			Error::SetStringFmt(error,
				"Failed to create private workload memory card '{}' (0x{:08x}).",
				temporary, static_cast<u32>(destination_fd));
			return false;
		}

		constexpr size_t COPY_BUFFER_SIZE = 64 * 1024;
		std::unique_ptr<u8[]> buffer(new (std::nothrow) u8[COPY_BUFFER_SIZE]);
		u64 copied = 0;
		bool copy_okay = buffer != nullptr;
		while (copy_okay && copied < source_size)
		{
			const SceSize request = static_cast<SceSize>(
				std::min<u64>(source_size - copied, COPY_BUFFER_SIZE));
			const SceSSize read = sceIoRead(source_fd, buffer.get(), request);
			if (read <= 0)
			{
				copy_okay = false;
				break;
			}
			size_t written = 0;
			while (written < static_cast<size_t>(read))
			{
				const SceSSize result = sceIoWrite(destination_fd,
					buffer.get() + written,
					static_cast<SceSize>(static_cast<size_t>(read) - written));
				if (result <= 0)
				{
					copy_okay = false;
					break;
				}
				written += static_cast<size_t>(result);
			}
			copied += static_cast<u64>(read);
		}
		u8 trailing = 0;
		if (copy_okay && sceIoRead(source_fd, &trailing, 1) != 0)
			copy_okay = false;
		if (copy_okay && sceIoSyncByFd(destination_fd, 0) < 0)
			copy_okay = false;
		const int source_close = sceIoClose(source_fd);
		const int destination_close = sceIoClose(destination_fd);
		if (!copy_okay || copied != source_size || source_close < 0 ||
			destination_close < 0)
		{
			FileSystem::DeleteFilePath(temporary.c_str());
			Error::SetStringFmt(error,
				"Failed bounded provisioning of private workload memory card '{}' ({}/{} bytes).",
				destination, copied, source_size);
			return false;
		}

		// A second launcher must never replace an already-existing private card.
		if (FileSystem::FileExists(destination.c_str()))
		{
			FileSystem::DeleteFilePath(temporary.c_str());
			return true;
		}
		if (sceIoRename(temporary.c_str(), destination.c_str()) < 0)
		{
			FileSystem::DeleteFilePath(temporary.c_str());
			Error::SetStringFmt(error,
				"Failed to publish private workload memory card '{}'.", destination);
			return false;
		}
		*provisioned = true;
		return true;
	}

	bool ConfigurePrivateWorkloadCards(WorkloadReplaySelection* workload,
		Error* error)
	{
		if (!workload->enabled)
			return true;
		if (!FileSystem::EnsureDirectoryExists(workload->private_card_dir.c_str(),
				true, error))
		{
			return false;
		}

		const std::string card1 = workload->private_card_dir + "/Mcd001.ps2";
		const std::string card2 = workload->private_card_dir + "/Mcd002.ps2";
		if (!ProvisionPrivateWorkloadCard(workload->card1_input_path, card1,
				&workload->card1_provisioned, error) ||
			!ProvisionPrivateWorkloadCard(workload->card2_input_path, card2,
				&workload->card2_provisioned, error) ||
			!VerifyFileIdentity("private workload memory card 1", card1,
				workload->card1_identity.bytes,
				workload->card1_identity.sha256.c_str(), error) ||
			!VerifyFileIdentity("private workload memory card 2", card2,
				workload->card2_identity.bytes,
				workload->card2_identity.sha256.c_str(), error))
		{
			return false;
		}

		EmuFolders::MemoryCards = workload->private_card_dir;
		EmuConfig.Mcd[0].Enabled = true;
		EmuConfig.Mcd[0].Filename = "Mcd001.ps2";
		EmuConfig.Mcd[0].Type = MemoryCardType::File;
		EmuConfig.Mcd[1].Enabled = true;
		EmuConfig.Mcd[1].Filename = "Mcd002.ps2";
		EmuConfig.Mcd[1].Type = MemoryCardType::File;
		Console.WriteLn(
			"VitaSX2 workload private cards: directory=%s card1_provisioned=%u card2_provisioned=%u.",
			workload->private_card_dir.c_str(),
			workload->card1_provisioned ? 1u : 0u,
			workload->card2_provisioned ? 1u : 0u);
		return true;
	}

	bool WriteAll(SceUID fd, const void* data, size_t size)
	{
		const u8* cursor = static_cast<const u8*>(data);
		while (size > 0)
		{
			const SceSSize written =
				sceIoWrite(fd, cursor, static_cast<SceSize>(size));
			if (written <= 0)
				return false;
			cursor += written;
			size -= static_cast<size_t>(written);
		}
		return true;
	}

	bool PublishStatus(const char* path, std::string_view contents)
	{
		const std::string temporary = std::string(path) + ".tmp";
		sceIoRemove(temporary.c_str());
		const SceUID fd = sceIoOpen(temporary.c_str(),
			SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
		if (fd < 0)
			return false;

		const bool wrote = WriteAll(fd, contents.data(), contents.size());
		const bool closed = sceIoClose(fd) >= 0;
		const bool okay = wrote && closed;
		if (!okay)
		{
			sceIoRemove(temporary.c_str());
			return false;
		}

		sceIoRemove(path);
		if (sceIoRename(temporary.c_str(), path) < 0)
		{
			sceIoRemove(temporary.c_str());
			return false;
		}
		return true;
	}

#if VITASX2_WORKLOAD_REPLAY_CHECKPOINT
	bool PublishWorkloadTerminal(
		const WorkloadReplaySelection& workload,
		const Pcsx2Trace::PortableReplayExternalDeviceAccessCounts& accesses,
		Error* error)
	{
		char marker[1024];
		const int length = std::snprintf(marker, sizeof(marker),
			"status=ok\nformat=vitasx2-workload-terminal-v1\nslug=%s\n"
			"checkpoint_projection=machine-checkpoint-v5\n"
			"configured_vsync_frames=%u\ncapture_vsync_frame=%u\n"
			"checkpoint_records=%llu\nee_pc=%08x\niop_pc=%08x\n"
			"ee_cycle=%llu\niop_cycle=%llu\n"
			"external_dev9_reads=%llu\nexternal_dev9_writes=%llu\n"
			"external_dev9_dma=%llu\nexternal_dev9_irq_scheduled=%llu\n"
			"external_dev9_irq_delivered=%llu\n"
			"external_firewire_reads=%llu\nexternal_firewire_writes=%llu\n"
			"external_firewire_irq=%llu\n",
			workload.slug.c_str(), workload.terminal_vsync_frames, g_FrameCount,
			static_cast<unsigned long long>(
				Pcsx2Trace::GetMachineCheckpointTraceRecordsWritten()),
			cpuRegs.pc, psxRegs.pc,
			static_cast<unsigned long long>(cpuRegs.cycle),
			static_cast<unsigned long long>(psxRegs.cycle),
			static_cast<unsigned long long>(accesses.dev9_reads),
			static_cast<unsigned long long>(accesses.dev9_writes),
			static_cast<unsigned long long>(accesses.dev9_dma),
			static_cast<unsigned long long>(accesses.dev9_irq_scheduled),
			static_cast<unsigned long long>(accesses.dev9_irq_delivered),
			static_cast<unsigned long long>(accesses.firewire_reads),
			static_cast<unsigned long long>(accesses.firewire_writes),
			static_cast<unsigned long long>(accesses.firewire_irq));
		if (length <= 0 || static_cast<size_t>(length) >= sizeof(marker) ||
			!PublishStatus(workload.terminal_done_path.c_str(),
				std::string_view(marker, static_cast<size_t>(length))))
		{
			Error::SetString(error,
				"Failed to publish the workload terminal checkpoint receipt.");
			return false;
		}
		return true;
	}
#endif

	bool PublishHarnessLaunchReceipt(Error* error)
	{
		// The harness publishes this request only after closing the foreground
		// application and freezing the exact SELF. Acknowledgement requires both
		// the fresh nonce and a byte-for-byte SHA-256 match against the installed
		// eboot.bin, so a truncated or same-size stale FTP replacement cannot be
		// mistaken for the requested product build.
		if (!FileSystem::FileExists(PRODUCT_LAUNCH_REQUEST_PATH))
			return true;
		std::string request;
		if (!ReadBoundedWorkloadText(PRODUCT_LAUNCH_REQUEST_PATH, 512,
				&request, error))
		{
			return false;
		}
		if (!request.ends_with("\n") || request.find('\r') != std::string::npos ||
			std::count(request.begin(), request.end(), '\n') != 4)
		{
			Error::SetString(error, "Malformed Vita harness launch request.");
			return false;
		}
		request.pop_back();
		std::array<std::string_view, 4> lines;
		size_t line_begin = 0;
		for (size_t i = 0; i < lines.size(); i++)
		{
			const size_t line_end = request.find('\n', line_begin);
			const size_t end = line_end == std::string::npos ?
				request.size() : line_end;
			lines[i] = std::string_view(request).substr(line_begin, end - line_begin);
			line_begin = end + 1;
		}
		constexpr std::string_view NONCE_KEY = "Nonce=";
		constexpr std::string_view SELF_BYTES_KEY = "SelfBytes=";
		constexpr std::string_view SELF_SHA256_KEY = "SelfSHA256=";
		u64 self_bytes = 0;
		if (lines[0] != "[VitaSX2LaunchV2]" ||
			!lines[1].starts_with(NONCE_KEY) ||
			!lines[2].starts_with(SELF_BYTES_KEY) ||
			!lines[3].starts_with(SELF_SHA256_KEY) ||
			!IsLowerHexSha256(lines[1].substr(NONCE_KEY.size())) ||
			!ParsePositiveU64(lines[2].substr(SELF_BYTES_KEY.size()), &self_bytes) ||
			!IsLowerHexSha256(lines[3].substr(SELF_SHA256_KEY.size())))
		{
			Error::SetString(error, "Malformed Vita harness launch request.");
			return false;
		}
		const std::string self_sha256(lines[3].substr(SELF_SHA256_KEY.size()));
		if (!VerifyFileIdentity("deployed SELF", PRODUCT_SELF_PATH, self_bytes,
				self_sha256.c_str(), error))
		{
			return false;
		}
		request.push_back('\n');
		if (!PublishStatus(PRODUCT_LAUNCH_RECEIPT_PATH,
				request))
		{
			Error::SetString(error, "Failed to publish the Vita harness launch receipt.");
			return false;
		}
		return true;
	}

	void RemoveOutput(const char* path)
	{
		sceIoRemove(path);
		const std::string temporary = std::string(path) + ".tmp";
		sceIoRemove(temporary.c_str());
	}

	bool RemoveValidationFileIfPresent(const char* path, Error* error)
	{
		const int result = sceIoRemove(path);
		if (result == 0 || result == PSP2_ERROR_ERRNO_ENOENT)
			return true;

		Error::SetStringFmt(error,
			"Failed to reset validation file '{}' (error={:08x}).", path,
			static_cast<u32>(result));
		return false;
	}

	bool EnsureDirectories(Error* error)
	{
		if (!FileSystem::EnsureDirectoryExists(DATA_DIR, false, error) ||
			!FileSystem::EnsureDirectoryExists(BIOS_DIR, false, error) ||
			!FileSystem::EnsureDirectoryExists(DISC_DIR, false, error))
		{
			return false;
		}

		const char* memory_card_dir = VITASX2_PRODUCT_BOOT_VALIDATION ?
			VALIDATION_MEMORY_CARD_DIR : PRODUCT_MEMORY_CARD_DIR;
		if (VITASX2_PRODUCT_BOOT_VALIDATION &&
			!FileSystem::EnsureDirectoryExists(VALIDATION_DIR, false, error))
		{
			return false;
		}
		if (VITASX2_PRODUCT_BOOT_VALIDATION &&
			!FileSystem::EnsureDirectoryExists(VALIDATION_BIOS_DIR, false, error))
		{
			return false;
		}
		if (!VITASX2_PRODUCT_BOOT_VALIDATION &&
			!FileSystem::EnsureDirectoryExists(ELF_DIR, false, error))
		{
			return false;
		}
		return FileSystem::EnsureDirectoryExists(memory_card_dir, false, error);
	}

	bool ReadConfiguredBootPath(std::string* boot_path,
		ConfiguredBootKind* boot_kind, Error* error)
	{
		if (VITASX2_PRODUCT_BOOT_VALIDATION)
		{
			*boot_path = VALIDATION_DISC_PATH;
			*boot_kind = ConfiguredBootKind::Disc;
			return true;
		}

		std::FILE* file = FileSystem::OpenCFile(BOOT_PATH_CONFIG, "rb");
		if (!file)
		{
			Error::SetStringFmt(error,
				"Missing VitaSX2 boot selector '{}'.", BOOT_PATH_CONFIG);
			return false;
		}

		char buffer[1024] = {};
		const bool read = std::fgets(buffer, sizeof(buffer), file) != nullptr;
		std::fclose(file);
		if (!read)
		{
			Error::SetStringFmt(error,
				"Failed to read VitaSX2 boot selector '{}'.", BOOT_PATH_CONFIG);
			return false;
		}

		size_t length = std::strlen(buffer);
		while (length > 0 &&
			(buffer[length - 1] == '\r' || buffer[length - 1] == '\n' ||
				buffer[length - 1] == ' ' || buffer[length - 1] == '\t'))
		{
			buffer[--length] = 0;
		}
		const std::string_view path(buffer, length);
		// PCSX2 owner: pcsx2-qt/MainWindow.cpp::onStartBIOSActionTriggered()
		// starts VMManager with no boot filename, while the command-line `-bios`
		// route selects CDVD_SourceType::NoDisc explicitly. Keep the selector
		// literal outside every filesystem namespace so BIOS boot cannot broaden
		// this unsafe home's file access.
		if (path == "bios")
		{
			boot_path->clear();
			*boot_kind = ConfiguredBootKind::Bios;
			return true;
		}
		// This unsafe-homebrew product only accepts user-provisioned images in
		// its own data directory. A malformed selector must never broaden reads
		// to the owner's other applications or savedata.
		constexpr std::string_view disc_prefix = "ux0:data/vitasx2/disc/";
		constexpr std::string_view elf_prefix = "ux0:data/vitasx2/elf/";
		const bool is_disc = path.starts_with(disc_prefix) &&
			path.size() > disc_prefix.size();
		const bool is_elf = path.starts_with(elf_prefix) &&
			path.size() > elf_prefix.size() && VMManager::IsElfFileName(path);
		if (!is_disc && !is_elf)
		{
			Error::SetString(error,
				"Boot selector must be 'bios', name one file below the VitaSX2 disc "
				"directory, or name a PCSX2-recognized ELF below the VitaSX2 ELF "
				"directory.");
			return false;
		}
		const std::string_view filename = path.substr(
			is_elf ? elf_prefix.size() : disc_prefix.size());
		if (filename == "." || filename == ".." ||
			filename.find('/') != std::string_view::npos ||
			filename.find('\\') != std::string_view::npos ||
			filename.find(':') != std::string_view::npos)
		{
			Error::SetString(error,
				"Boot selector must name a single file without traversal or subdirectories.");
			return false;
		}
		boot_path->assign(path.data(), path.size());
		*boot_kind = is_elf ? ConfiguredBootKind::Elf : ConfiguredBootKind::Disc;
		return true;
	}

	void ConfigureProductSettings()
	{
		// Until the settings frontend lands, begin from PCSX2's defaults and
		// override only target invariants and unsupported desktop mechanisms.
		EmuConfig = Pcsx2Config();
		EmuConfig.EnableFastBoot = true;
		EmuConfig.EnableFastBootFastForward = false;
		EmuConfig.EnablePatches = false;
		EmuConfig.EnableCheats = false;
		EmuConfig.EnableWideScreenPatches = false;
		EmuConfig.EnableNoInterlacingPatches = false;
		EmuConfig.EnableDiscordPresence = false;
		EmuConfig.EnablePINE = false;
		EmuConfig.EnableGameFixes = false;
		EmuConfig.Gamefixes.DisableAll();
		EmuConfig.HostFs = false;
		EmuConfig.CurrentGameArgs.clear();

		EmuConfig.Cpu.Recompiler.EnableEE = true;
		EmuConfig.Cpu.Recompiler.EnableIOP = true;
		EmuConfig.Cpu.Recompiler.EnableVU0 = true;
		EmuConfig.Cpu.Recompiler.EnableVU1 = true;
		EmuConfig.Cpu.Recompiler.EnableFastmem = false;
		EmuConfig.Cpu.Recompiler.EnableEECache = false;
		// Maximum VU1 tier: select PCSX2's existing nearest-rounding contract so
		// the Cortex-A9 provider can use its fixed-nearest Advanced SIMD FMAC
		// datapath. Product boot/oracle validation retains PS2 chop mode below by
		// construction; the scalar VFP path also remains available whenever VU1's
		// configured FPCR is not nearest+FZ with standard overflow clamping.
		if (!VITASX2_PRODUCT_BOOT_VALIDATION)
			EmuConfig.Cpu.VU1FPCR.SetRoundMode(FPRoundMode::Nearest);
		// Performance VU1 tier. PCSX2 owners:
		// x86/microVU_Flags.inl::mVUsetFlags() and
		// VU1micro.cpp::vu1ExecMicro(), plus VUops.cpp's implicit stall tests.
		// State these explicitly instead of
		// inheriting constructor defaults so the Vita product contract remains
		// visible when upstream defaults change. Deterministic oracle validation
		// below disables every speedhack and therefore retains the accurate tier.
		EmuConfig.Speedhacks.vuFlagHack = true;
		EmuConfig.Speedhacks.vu1Instant = true;
		// PS2 VU microprograms are statically scheduled. Preserve explicit
		// WAITQ/WAITP synchronization, result publication, and flag timing, but
		// do not spend Cortex-A9 instructions proving that automatic FMAC,
		// FDIV/EFU-resource, branch-after-IALU stalls, or the old-VI branch
		// compatibility window are absent at runtime.
		EmuConfig.Speedhacks.vu1AssumeScheduled = true;
		// Sony VU User Manual 3.4.5/3.4.6 permits unsynchronized consumers to
		// observe the old Q/P value. The Performance tier deliberately assumes
		// microcode wants the newly computed value immediately, eliminating the
		// pending FDIV/EFU timestamp and readiness machinery around scalar VFP.
		EmuConfig.Speedhacks.vu1InstantQP = true;
		// Maximum-tier arithmetic approximations. ARM ARM A8.6.371-372 and
		// A8.6.378-379 define the Advanced SIMD reciprocal/reciprocal-square-root
		// estimate and Newton refinement operations; the Cortex-A9 MPE TRM table
		// 3-8 gives every D-form step one issue cycle. The generated paths retain
		// Sony DIV D/I flags, zero exceptional results, vuDouble()/vuFloat()
		// normalization, and Q/P visibility, while deliberately replacing the
		// finite exact divide/square-root arithmetic. Accurate boot validation's
		// DisableAll() below keeps the exact scalar VFP implementation.
		// Approximate Q caused visible refmap corruption for negligible speed
		// gain on real Vita hardware. Keep exact Q in the playable default.
		EmuConfig.Speedhacks.vu1ApproximateQ = false;
		EmuConfig.Speedhacks.vu1ApproximateP = true;
		// PCSX2 owner: Pcsx2Config::SpeedhackOptions::MTVU and
		// VMManager::SetEmuThreadAffinities(). Normal Vita execution overlaps
		// VU1 micro work with EE/IOP on the three documented application cores.
		// Oracle validation remains single-threaded so every checkpoint is an
		// immediately quiescent architectural boundary.
		EmuConfig.Speedhacks.vuThread = !VITASX2_PRODUCT_BOOT_VALIDATION;
		EmuConfig.DEV9.EthEnable = false;
		EmuConfig.DEV9.HddEnable = false;

		for (u32 port = 0; port < Pad::NUM_CONTROLLER_PORTS; port++)
			EmuConfig.Pad.Ports[port].Type = Pad::ControllerType::NotConnected;
		if (!VITASX2_PRODUCT_BOOT_VALIDATION)
			EmuConfig.Pad.Ports[0].Type = Pad::ControllerType::DualShock2;

		// VitaGxmGsState consumes PCSX2's decoded GSState boundary directly. The
		// enum remains SW because PCSX2 has no Vita renderer enum; hardware-cache
		// behavior is reported separately by GSIsHardwareRenderer().
		EmuConfig.GS.Renderer = GSRendererType::SW;
		// PCSX2 owner: MTGS.cpp. Normal product execution overlaps the EE and GS
		// on separate Cortex-A9 cores; oracle validation remains deliberately
		// synchronous so every trace boundary is immediately quiescent.
		EmuConfig.GS.SynchronousMTGS = VITASX2_PRODUCT_BOOT_VALIDATION;
		EmuConfig.GS.VsyncEnable = false;

		EmuConfig.SPU2.Backend = VITASX2_PRODUCT_BOOT_VALIDATION ?
			AudioBackend::Null : AudioBackend::Cubeb;
		// VitaAudioStream is a native low-latency ring and deliberately does not
		// implement SoundTouch. Do not request the unsupported desktop stretcher.
		EmuConfig.SPU2.SyncMode =
			Pcsx2Config::SPU2Options::SPU2SyncMode::Disabled;

		if (VITASX2_PRODUCT_BOOT_VALIDATION)
		{
			// Mirrors pcsx2-trace/Main.cpp::ConfigureDeterministicSettings().
			EmuConfig.ManuallySetRealTimeClock = true;
			EmuConfig.RtcYear = 20;
			EmuConfig.RtcMonth = 3;
			EmuConfig.RtcDay = 4;
			EmuConfig.RtcHour = 0;
			EmuConfig.RtcMinute = 0;
			EmuConfig.RtcSecond = 0;
			EmuConfig.Speedhacks.DisableAll();
		}
	}

	void ConfigureProductPerformanceTelemetry()
	{
		bool enabled = VITASX2_PRODUCT_BOOT_VALIDATION;
		bool cpu_stage_profiler_enabled = false;
		const char* source = VITASX2_PRODUCT_BOOT_VALIDATION ?
			"boot-validation" : "default";
		if (!VITASX2_PRODUCT_BOOT_VALIDATION &&
			FileSystem::FileExists(PRODUCT_CONFIG_PATH))
		{
			INISettingsInterface settings(PRODUCT_CONFIG_PATH);
			if (settings.Load())
			{
				constexpr const char* section = "Diagnostics";
				constexpr const char* key = "EnablePerformanceTelemetry";
				if (settings.ContainsValue(section, key) &&
					!settings.GetBoolValue(section, key, &enabled))
				{
					enabled = false;
					Console.Warning(
						"VitaSX2 ignored malformed [%s] %s in %s; performance telemetry remains disabled.",
						section, key, PRODUCT_CONFIG_PATH);
				}
				constexpr const char* profiler_key =
					"EnableCpuStageProfiler";
				if (settings.ContainsValue(section, profiler_key) &&
					!settings.GetBoolValue(section, profiler_key,
						&cpu_stage_profiler_enabled))
				{
					cpu_stage_profiler_enabled = false;
					Console.Warning(
						"VitaSX2 ignored malformed [%s] %s in %s.",
						section, profiler_key, PRODUCT_CONFIG_PATH);
				}
#if !defined(VITASX2_CPU_PROFILER)
				if (cpu_stage_profiler_enabled)
				{
					Console.Warning(
						"VitaSX2 ignored [Diagnostics] EnableCpuStageProfiler because this product was built without VITASX2_CPU_PROFILER.");
					cpu_stage_profiler_enabled = false;
				}
#endif
				source = PRODUCT_CONFIG_PATH;
			}
			else
			{
				Console.Warning(
					"VitaSX2 could not parse %s; performance telemetry remains disabled.",
					PRODUCT_CONFIG_PATH);
			}
		}

		// The flag is immutable after this point. PCSX2's worker threads are
		// created later by CPUThreadInitialize()/OpenGS(), so their plain reads
		// observe this startup configuration without hot-path atomic traffic.
		VitaPerformanceTelemetry::SetEnabledBeforeVmStart(enabled);
		if (!enabled)
			cpu_stage_profiler_enabled = false;
		VitaPerformanceTelemetry::ConfigureCpuStageProfilerBeforeVmStart(
			cpu_stage_profiler_enabled);
		Console.WriteLn(
			"VitaSX2 performance telemetry: enabled=%u cpu_stage_profiler=%u source=%s.",
			enabled ? 1u : 0u,
			cpu_stage_profiler_enabled ? 1u : 0u,
			source);
	}

	void ConfigureProductInputAutomation(const WorkloadReplaySelection& workload)
	{
		constexpr u32 DEFAULT_PRESSED_FRAMES = 2;
		constexpr u32 DEFAULT_RELEASED_FRAMES = 6;
		constexpr u32 MAX_CADENCE_FRAMES = 600;
		InputManager::VitaPadAutoFireButton button =
			InputManager::VitaPadAutoFireButton::None;
		u32 pressed_frames = DEFAULT_PRESSED_FRAMES;
		u32 released_frames = DEFAULT_RELEASED_FRAMES;
		const char* button_name = "None";
		const char* source = "default";
		bool accept_physical_input = true;

		if (workload.enabled)
		{
			button = workload.input_button;
			pressed_frames = workload.input_pressed_frames;
			released_frames = workload.input_released_frames;
			button_name = button == InputManager::VitaPadAutoFireButton::Cross ?
				"Cross" : (button == InputManager::VitaPadAutoFireButton::Circle ?
					"Circle" : "None");
			source = workload.manifest_path.c_str();
			accept_physical_input = false;
		}
		else if (!VITASX2_PRODUCT_BOOT_VALIDATION &&
			FileSystem::FileExists(PRODUCT_CONFIG_PATH))
		{
			INISettingsInterface settings(PRODUCT_CONFIG_PATH);
			if (settings.Load())
			{
				constexpr const char* section = "InputAutomation";
				const std::string configured_button =
					settings.GetStringValue(section, "AutoFireButton", "None");
				if (StringUtil::Strcasecmp(configured_button.c_str(), "Cross") == 0)
				{
					button = InputManager::VitaPadAutoFireButton::Cross;
					button_name = "Cross";
				}
				else if (StringUtil::Strcasecmp(configured_button.c_str(), "Circle") == 0)
				{
					button = InputManager::VitaPadAutoFireButton::Circle;
					button_name = "Circle";
				}
				else if (StringUtil::Strcasecmp(configured_button.c_str(), "None") != 0)
				{
					Console.Warning(
						"VitaSX2 ignored unknown [InputAutomation] AutoFireButton '%s' in %s.",
						configured_button.c_str(), PRODUCT_CONFIG_PATH);
				}

				bool cadence_valid = true;
				if (button != InputManager::VitaPadAutoFireButton::None)
				{
					if (settings.ContainsValue(section, "AutoFirePressedFrames") &&
						(!settings.GetUIntValue(section, "AutoFirePressedFrames", &pressed_frames) ||
							pressed_frames == 0 || pressed_frames > MAX_CADENCE_FRAMES))
					{
						cadence_valid = false;
					}
					if (settings.ContainsValue(section, "AutoFireReleasedFrames") &&
						(!settings.GetUIntValue(section, "AutoFireReleasedFrames", &released_frames) ||
							released_frames == 0 || released_frames > MAX_CADENCE_FRAMES))
					{
						cadence_valid = false;
					}
				}
				if (!cadence_valid)
				{
					Console.Warning(
						"VitaSX2 disabled malformed [InputAutomation] autofire cadence in %s.",
						PRODUCT_CONFIG_PATH);
					button = InputManager::VitaPadAutoFireButton::None;
					button_name = "None";
					pressed_frames = DEFAULT_PRESSED_FRAMES;
					released_frames = DEFAULT_RELEASED_FRAMES;
				}
				source = PRODUCT_CONFIG_PATH;
			}
			else
			{
				Console.Warning(
					"VitaSX2 could not parse %s; input automation remains disabled.",
					PRODUCT_CONFIG_PATH);
			}
		}

		if (!InputManager::ConfigureVitaPadAutoFire(
				button, pressed_frames, released_frames, accept_physical_input))
		{
			button = InputManager::VitaPadAutoFireButton::None;
			button_name = "None";
			InputManager::ConfigureVitaPadAutoFire(button, 1, 1,
				accept_physical_input);
		}
		Console.WriteLn(
			"VitaSX2 input autofire: button=%s pressed_frames=%u released_frames=%u "
			"start=%s physical_input=%u source=%s.",
			button_name, pressed_frames, released_frames,
			workload.enabled ? "workload-replay" : "game-elf",
			accept_physical_input ? 1u : 0u, source);
	}

	bool NativeProvidersSelected()
	{
		return Cpu == &recCpu && psxCpu == &psxRec &&
			CpuVU0 == static_cast<BaseVUmicroCPU*>(&CpuMicroVU0) &&
			CpuVU1 == static_cast<BaseVUmicroCPU*>(&CpuMicroVU1);
	}

#if VITASX2_PRODUCT_BOOT_VALIDATION
	struct ValidationProgressState
	{
		u32 entry_frame = 0;
		u32 last_frame_delta = 0;
		u32 next_sequence = 0;
		u32 sampled_peak_heap_used = 0;
		u32 sampled_minimum_heap_free = 0;
		u32 sampled_minimum_heap_headroom = 0;
		u32 sampled_minimum_lpddr_free = 0;
		bool has_heap_sample = false;
		bool has_lpddr_sample = false;
		bool active = false;
		bool write_failed = false;
	};

	ValidationProgressState s_validation_progress;

	struct ValidationMemorySnapshot
	{
		u32 heap_limit = 0;
		u32 heap_arena = 0;
		u32 heap_used = 0;
		u32 heap_free = 0;
		u32 heap_free_chunks = 0;
		u32 heap_top_free = 0;
		u32 heap_sbrk_remaining = 0;
		u32 heap_headroom = 0;
		u32 lpddr_free = 0;
		s32 lpddr_result = 0;
	};

	ValidationMemorySnapshot CaptureValidationMemory()
	{
		const struct mallinfo heap = mallinfo();
		ValidationMemorySnapshot snapshot;
		snapshot.heap_limit = _newlib_heap_size_user;
		snapshot.heap_arena = static_cast<u32>(heap.arena);
		snapshot.heap_used = static_cast<u32>(heap.uordblks);
		snapshot.heap_free = static_cast<u32>(heap.fordblks);
		snapshot.heap_free_chunks = static_cast<u32>(heap.ordblks);
		snapshot.heap_top_free = static_cast<u32>(heap.keepcost);
		snapshot.heap_sbrk_remaining =
			snapshot.heap_limit > snapshot.heap_arena ?
				snapshot.heap_limit - snapshot.heap_arena : 0;
		snapshot.heap_headroom =
			snapshot.heap_sbrk_remaining + snapshot.heap_free;

		SceKernelFreeMemorySizeInfo free_info = {};
		free_info.size = sizeof(free_info);
		snapshot.lpddr_result = sceKernelGetFreeMemorySize(&free_info);
		if (snapshot.lpddr_result >= 0)
			snapshot.lpddr_free = static_cast<u32>(free_info.size_user);
		return snapshot;
	}

	void AccumulateValidationMemory(const ValidationMemorySnapshot& memory)
	{
		if (!s_validation_progress.has_heap_sample)
		{
			s_validation_progress.sampled_minimum_heap_free = memory.heap_free;
			s_validation_progress.sampled_minimum_heap_headroom =
				memory.heap_headroom;
			s_validation_progress.has_heap_sample = true;
		}
		s_validation_progress.sampled_peak_heap_used = std::max(
			s_validation_progress.sampled_peak_heap_used, memory.heap_used);
		s_validation_progress.sampled_minimum_heap_free = std::min(
			s_validation_progress.sampled_minimum_heap_free, memory.heap_free);
		s_validation_progress.sampled_minimum_heap_headroom = std::min(
			s_validation_progress.sampled_minimum_heap_headroom,
			memory.heap_headroom);
		if (memory.lpddr_result >= 0)
		{
			if (!s_validation_progress.has_lpddr_sample)
			{
				s_validation_progress.sampled_minimum_lpddr_free =
					memory.lpddr_free;
				s_validation_progress.has_lpddr_sample = true;
			}
			s_validation_progress.sampled_minimum_lpddr_free = std::min(
				s_validation_progress.sampled_minimum_lpddr_free,
				memory.lpddr_free);
		}
	}

	bool AppendValidationProgress(std::string_view line, bool first_record)
	{
		const SceUID fd = sceIoOpen(VALIDATION_PROGRESS_PATH,
			SCE_O_WRONLY | SCE_O_CREAT |
				(first_record ? SCE_O_TRUNC : SCE_O_APPEND),
			0666);
		if (fd < 0)
			return false;

		const bool wrote = WriteAll(fd, line.data(), line.size());
		// Official PSP2 iofilemgr ownership: synchronize this closed-record
		// receipt to storage so a later forced reboot can still locate the last
		// completed guest-frame milestone.
		const bool synced = wrote && sceIoSyncByFd(fd, 0) >= 0;
		const bool closed = sceIoClose(fd) >= 0;
		return wrote && synced && closed;
	}

	bool RecordValidationProgress(const char* phase, u32 frame_delta,
		bool first_record = false)
	{
		const VitaA32EeProviderStats ee = VitaGetA32EeProviderStats();
		const VitaA32EeProviderStats ee_session =
			VitaGetA32EeSessionFallbackStats();
		const VitaA32IopProviderStats iop = VitaGetA32IopProviderStats();
		const VitaVU::Vu1ProviderStats vu0 = VitaVU::GetVu0ProviderStats();
		const VitaVU::Vu1ProviderStats vu1 = VitaVU::GetVu1ProviderStats();
		const ValidationMemorySnapshot memory = CaptureValidationMemory();
		AccumulateValidationMemory(memory);

		char text[1024];
		const int length = std::snprintf(text, sizeof(text),
			"v=1 seq=%u phase=%s frame=%u frame_delta=%u "
			"ee_pc=%08x ee_cycle=%llu ee_records=%llu checkpoint_records=%llu "
			"ee_blocks=%u ee_instructions=%u ee_interpreter=%u ee_failed=%u "
			"iop_pc=%08x iop_entries=%llu iop_interpreter=%u iop_failed=%u "
			"vu0_pairs=%llu vu0_interpreter=%llu vu0_failed=%u "
			"vu1_tpc=%03x vu1_pairs=%llu vu1_interpreter=%llu vu1_failed=%u "
			"heap_limit=%u heap_arena=%u heap_used=%u heap_free=%u "
			"heap_chunks=%u heap_top=%u heap_sbrk_remaining=%u heap_headroom=%u "
			"heap_sampled_peak=%u heap_sampled_min_free=%u "
			"heap_sampled_min_headroom=%u lpddr_free=%u "
			"lpddr_sampled_min_free=%u "
			"lpddr_result=%08x\n",
			s_validation_progress.next_sequence, phase, g_FrameCount, frame_delta,
			cpuRegs.pc, static_cast<unsigned long long>(cpuRegs.cycle),
			static_cast<unsigned long long>(Pcsx2Trace::GetEeTraceRecordsWritten()),
			static_cast<unsigned long long>(
				Pcsx2Trace::GetMachineCheckpointTraceRecordsWritten()),
			ee.compiled_blocks, ee.compiled_instructions,
			ee_session.interpreter_steps, ee_session.failed_blocks,
			psxRegs.pc, static_cast<unsigned long long>(iop.executed_blocks),
			iop.interpreter_blocks, iop.failed_blocks,
			static_cast<unsigned long long>(vu0.executed_pairs),
			static_cast<unsigned long long>(vu0.interpreter_steps),
			vu0.compile_failures, VU1.VI[REG_TPC].UL,
			static_cast<unsigned long long>(vu1.executed_pairs),
			static_cast<unsigned long long>(vu1.interpreter_steps),
			vu1.compile_failures, memory.heap_limit, memory.heap_arena,
			memory.heap_used, memory.heap_free, memory.heap_free_chunks,
			memory.heap_top_free, memory.heap_sbrk_remaining,
			memory.heap_headroom,
			s_validation_progress.sampled_peak_heap_used,
			s_validation_progress.sampled_minimum_heap_free,
			s_validation_progress.sampled_minimum_heap_headroom,
			memory.lpddr_free,
			s_validation_progress.sampled_minimum_lpddr_free,
			static_cast<u32>(memory.lpddr_result));
		if (length <= 0 || static_cast<size_t>(length) >= sizeof(text) ||
			!AppendValidationProgress(
				std::string_view(text, static_cast<size_t>(length)), first_record))
		{
			return false;
		}

		Console.WriteLn(
			"VitaSX2 progress receipt %u: phase=%s frame=%u delta=%u ee=%u iop=%llu vu1_pairs=%llu heap=%u/%u headroom=%u lpddr_free=%u.",
			s_validation_progress.next_sequence, phase, g_FrameCount, frame_delta,
			ee.compiled_instructions,
			static_cast<unsigned long long>(iop.executed_blocks),
			static_cast<unsigned long long>(vu1.executed_pairs), memory.heap_used,
			memory.heap_arena, memory.heap_headroom, memory.lpddr_free);
		s_validation_progress.next_sequence++;
		s_validation_progress.last_frame_delta = frame_delta;
		return true;
	}

	void RecordValidationVSyncProgress()
	{
		if (!s_validation_progress.active)
			return;
		const u32 frame_delta =
			static_cast<u32>(g_FrameCount - s_validation_progress.entry_frame);
		if (frame_delta == 0 ||
			(frame_delta % VALIDATION_PROGRESS_FRAME_INTERVAL) != 0 ||
			frame_delta == s_validation_progress.last_frame_delta)
		{
			return;
		}

		if (RecordValidationProgress("frame", frame_delta))
			return;

		s_validation_progress.write_failed = true;
		s_validation_progress.active = false;
		Console.Error("VitaSX2 failed to synchronize its frame-progress receipt.");
		VMManager::SetState(VMState::Stopping);
		if (Cpu)
			Cpu->ExitExecution();
	}

	bool StartValidationProgress()
	{
		s_validation_progress = {};
		s_validation_progress.entry_frame = g_FrameCount;
		// Truncate here rather than relying on the startup's best-effort stale-file
		// removal. A failed truncate is terminal; a new run must never append its
		// sequence zero to receipts from an older process.
		if (!RecordValidationProgress("entry", 0, true))
		{
			s_validation_progress.write_failed = true;
			return false;
		}
		s_validation_progress.active = true;
		VitaSetVSyncProgressCallback(RecordValidationVSyncProgress);
		return true;
	}

	void StopValidationProgress()
	{
		VitaSetVSyncProgressCallback(nullptr);
		s_validation_progress.active = false;
	}

	bool SealValidationCheckpointProgress()
	{
		const u32 frame_delta =
			static_cast<u32>(g_FrameCount - s_validation_progress.entry_frame);
		if (s_validation_progress.last_frame_delta >=
			VALIDATION_AFTER_VSYNC_FRAMES)
		{
			return true;
		}

		// Counters.cpp increments g_FrameCount at VSyncEnd. A VU1 completion can
		// therefore reach the shared event-test checkpoint before the following
		// VSyncStart callback observes delta 1280. Seal exactly that legitimate
		// ordering here at the natural EE boundary which observed the checkpoint.
		if (frame_delta != VALIDATION_AFTER_VSYNC_FRAMES ||
			!RecordValidationProgress("checkpoint", frame_delta))
		{
			s_validation_progress.write_failed = true;
			return false;
		}
		return true;
	}

	bool NativeMilestoneHasNoFallback(std::string* evidence,
		const VitaGS::CanonicalRingValidationResult& gs_ring)
	{
		const VitaA32EeProviderStats ee = VitaGetA32EeProviderStats();
		const VitaA32EeProviderStats ee_session =
			VitaGetA32EeSessionFallbackStats();
		const VitaA32IopProviderStats iop = VitaGetA32IopProviderStats();
		const VitaVU::Vu1ProviderStats vu0 = VitaVU::GetVu0ProviderStats();
		const VitaVU::Vu1ProviderStats vu1 = VitaVU::GetVu1ProviderStats();
		const ValidationMemorySnapshot memory = CaptureValidationMemory();
		AccumulateValidationMemory(memory);

		char text[2048];
		const int length = std::snprintf(text, sizeof(text),
			"status=ok\nserial=%s\nelf=%s\ncrc=%08x\nentry=%08x\n"
			"ee_records=%llu\ncheckpoint_records=%llu\nframe_count=%u\n"
			"progress_records=%u\nprogress_frame_delta=%u\n"
			"ee_blocks=%u\nee_instructions=%u\nee_interpreter=%u\nee_failed=%u\n"
			"ee_session_interpreter=%u\nee_session_failed=%u\n"
			"ee_session_scan_unsupported=%u\nee_session_scan_boundary=%u\n"
			"ee_session_trace_branch_likely=%u\nee_session_execute_failed=%u\n"
			"ee_session_interpreter_path=%u\n"
			"iop_entries=%llu\niop_interpreter=%u\niop_failed=%u\n"
			"vu0_blocks=%llu\nvu0_pairs=%llu\nvu0_interpreter=%llu\nvu0_failed=%u\n"
			"vu1_blocks=%llu\nvu1_pairs=%llu\nvu1_interpreter=%llu\nvu1_failed=%u\n"
			"heap_limit=%u\nheap_arena=%u\nheap_used=%u\nheap_free=%u\n"
			"heap_free_chunks=%u\nheap_top_free=%u\nheap_sbrk_remaining=%u\n"
			"heap_headroom=%u\nheap_sampled_peak=%u\n"
			"heap_sampled_min_free=%u\nheap_sampled_min_headroom=%u\n"
			"lpddr_free=%u\nlpddr_sampled_min_free=%u\nlpddr_result=%08x\n"
			"gs_ring_canonical_bytes=%u\ngs_ring_packet_qwc=%u\n"
			"gs_ring_packet_hash=%016llx\ngs_ring_local_hash=%016llx\n"
			"gs_ring_pixel_checks=%u\ngs_ring_address_checks=%u\n"
			"gs_ring_clut_cases=%u\ngs_ring_readback_checks=%u\n"
			"gs_ring_reopened_hash=%016llx\ngs_ring_reopened_clean=%u\n",
			VMManager::GetDiscSerial().c_str(), VMManager::GetDiscELF().c_str(),
			VMManager::GetDiscCRC(), VMManager::Internal::GetCurrentELFEntryPoint(),
			static_cast<unsigned long long>(Pcsx2Trace::GetEeTraceRecordsWritten()),
			static_cast<unsigned long long>(
				Pcsx2Trace::GetMachineCheckpointTraceRecordsWritten()),
			g_FrameCount, s_validation_progress.next_sequence,
			s_validation_progress.last_frame_delta,
			ee.compiled_blocks, ee.compiled_instructions, ee.interpreter_steps,
			ee.failed_blocks, ee_session.interpreter_steps,
			ee_session.failed_blocks,
			ee_session.scan_unsupported_fallbacks,
			ee_session.scan_boundary_fallbacks,
			ee_session.exact_trace_branch_likely_fallbacks,
			ee_session.execute_failed_fallbacks,
			ee_session.interpreter_path_fallbacks,
			static_cast<unsigned long long>(iop.executed_blocks),
			iop.interpreter_blocks, iop.failed_blocks,
			static_cast<unsigned long long>(vu0.executed_blocks),
			static_cast<unsigned long long>(vu0.executed_pairs),
			static_cast<unsigned long long>(vu0.interpreter_steps), vu0.compile_failures,
			static_cast<unsigned long long>(vu1.executed_blocks),
			static_cast<unsigned long long>(vu1.executed_pairs),
			static_cast<unsigned long long>(vu1.interpreter_steps), vu1.compile_failures,
			memory.heap_limit, memory.heap_arena, memory.heap_used,
			memory.heap_free, memory.heap_free_chunks, memory.heap_top_free,
			memory.heap_sbrk_remaining, memory.heap_headroom,
			s_validation_progress.sampled_peak_heap_used,
			s_validation_progress.sampled_minimum_heap_free,
			s_validation_progress.sampled_minimum_heap_headroom,
			memory.lpddr_free,
			s_validation_progress.sampled_minimum_lpddr_free,
			static_cast<u32>(memory.lpddr_result),
			gs_ring.canonical_bytes, gs_ring.packet_qwc,
			static_cast<unsigned long long>(gs_ring.packet_hash),
			static_cast<unsigned long long>(gs_ring.local_hash),
			gs_ring.pixel_checks, gs_ring.address_checks, gs_ring.clut_cases,
			gs_ring.readback_checks,
			static_cast<unsigned long long>(gs_ring.reopened_hash),
			gs_ring.reopened_clean ? 1u : 0u);
		if (length <= 0 || static_cast<size_t>(length) >= sizeof(text))
			return false;
		evidence->assign(text, static_cast<size_t>(length));

		const bool ee_only_used_trace_steps =
			ee_session.interpreter_steps ==
				ee_session.exact_trace_branch_likely_fallbacks &&
			ee_session.scan_unsupported_fallbacks == 0 &&
			ee_session.scan_boundary_fallbacks == 0 &&
			ee_session.execute_failed_fallbacks == 0 &&
			ee_session.interpreter_path_fallbacks == 0;
		return NativeProvidersSelected() &&
			Pcsx2Trace::GetMachineCheckpointTraceRecordsWritten() ==
				VALIDATION_CHECKPOINT_RECORDS &&
			g_FrameCount == VALIDATION_EXPECTED_CAPTURE_FRAME &&
			!s_validation_progress.write_failed &&
			s_validation_progress.next_sequence == 11 &&
			s_validation_progress.last_frame_delta ==
				VALIDATION_AFTER_VSYNC_FRAMES &&
			ee.compiled_blocks > 0 &&
			iop.executed_blocks > 0 && ee_only_used_trace_steps &&
			ee_session.failed_blocks == 0 && iop.interpreter_blocks == 0 &&
			iop.failed_blocks == 0 && vu0.interpreter_steps == 0 &&
			vu0.compile_failures == 0 && vu1.executed_blocks > 0 &&
			vu1.executed_pairs > 0 && vu1.interpreter_steps == 0 &&
			vu1.compile_failures == 0 && gs_ring.reopened_clean;
	}
#endif

	int ExitProduct(int code)
	{
		sceKernelExitProcess(code);
		return code;
	}
} // namespace

int main()
{
	sceClibPrintf("VitaSX2: product startup validation=%u\n",
		VITASX2_PRODUCT_BOOT_VALIDATION ? 1u : 0u);

	Error error;
	bool cpu_thread_initialized = false;
	bool trace_started = false;
	bool vm_initialized = false;
	WorkloadReplaySelection workload;
#if VITASX2_WORKLOAD_REPLAY_CHECKPOINT
	bool workload_checkpoint_started = false;
	bool workload_checkpoint_complete = false;
	bool workload_external_window = false;
#endif
#if VITASX2_PRODUCT_BOOT_VALIDATION
	bool entry_trace_complete = false;
	bool checkpoint_started = false;
	VitaGS::CanonicalRingValidationResult gs_ring_validation;
#endif
	const char* log_path = VITASX2_PRODUCT_BOOT_VALIDATION ?
		VALIDATION_LOG_PATH : PRODUCT_LOG_PATH;
	const char* initialized_path = VITASX2_PRODUCT_BOOT_VALIDATION ?
		VALIDATION_INITIALIZED_PATH : PRODUCT_INITIALIZED_PATH;
	const char* failed_path = VITASX2_PRODUCT_BOOT_VALIDATION ?
		VALIDATION_FAILED_PATH : PRODUCT_FAILED_PATH;

	if (!EnsureDirectories(&error))
		goto fail;
	if (!PublishHarnessLaunchReceipt(&error))
		goto fail;
	Host::Internal::SetBaseSettingsLayer(&s_base_settings);
	Host::Internal::SetSecretsSettingsLayer(&s_secrets_settings);

	RemoveOutput(log_path);
	RemoveOutput(initialized_path);
	RemoveOutput(failed_path);
	if (VITASX2_PRODUCT_BOOT_VALIDATION)
	{
		RemoveOutput(VALIDATION_TRACE_PATH);
		RemoveOutput(VALIDATION_CHECKPOINT_PATH);
		RemoveOutput(VALIDATION_PROGRESS_PATH);
		RemoveOutput(VALIDATION_DONE_PATH);
		// This build owns an isolated pair of throwaway cards. Recreate them so
		// the physical boot and x86 oracle begin with the same SIO2 state; normal
		// product cards are never touched.
		if (!RemoveValidationFileIfPresent(VALIDATION_CARD_1, &error) ||
			!RemoveValidationFileIfPresent(VALIDATION_CARD_2, &error))
		{
			goto fail;
		}
	}

	Log::SetConsoleOutputLevel(LOGLEVEL_INFO);
	if (!Log::SetFileOutputLevel(
			VITASX2_PRODUCT_BOOT_VALIDATION ? LOGLEVEL_DEV : LOGLEVEL_INFO,
			log_path))
	{
		Error::SetString(&error, "Failed to open the VitaSX2 product log.");
		goto fail;
	}
	Console.WriteLn("%s product host starting (boot validation=%u).", NAME,
		VITASX2_PRODUCT_BOOT_VALIDATION ? 1u : 0u);

	EmuFolders::DataRoot = DATA_DIR;
	EmuFolders::Bios = VITASX2_PRODUCT_BOOT_VALIDATION ?
		VALIDATION_BIOS_DIR : BIOS_DIR;
	EmuFolders::MemoryCards = VITASX2_PRODUCT_BOOT_VALIDATION ?
		VALIDATION_MEMORY_CARD_DIR : PRODUCT_MEMORY_CARD_DIR;
	ConfigureProductPerformanceTelemetry();
	ConfigureProductSettings();
	if (!ReadOptionalWorkloadReplay(&workload, &error))
		goto fail;
#if VITASX2_WORKLOAD_REPLAY_CHECKPOINT
	if (!workload.enabled)
	{
		Error::SetString(&error,
			"The workload checkpoint build requires an authenticated active workload.");
		goto fail;
	}
#endif
	ConfigureProductInputAutomation(workload);
	if (!VerifyConfiguredBiosBundle(
			workload.enabled || VITASX2_PRODUCT_BOOT_VALIDATION, &error))
	{
		goto fail;
	}
	if (!ConfigurePrivateWorkloadCards(&workload, &error))
		goto fail;
	// PCSX2's _DynGen_DispatcherEvent() calls the event owner and falls directly
	// into register dispatch. Normal product execution can use the equivalent
	// persistent A32 path; bounded validation keeps its natural event callbacks.
	VitaSetA32EeInFrameEventResumeEnabled(!VITASX2_PRODUCT_BOOT_VALIDATION);
	// PCSX2's MTGS ownership expects the CPU and GS producers to execute in
	// parallel. Keep the product CPU thread on user core 0; the mailbox pins its
	// sole GXM-owning worker to core 1, and PCSX2's software raster worker uses
	// core 2. CPU3 remains reserved for the system and plugins. Sony's
	// thread-manager API treats a rejected affinity as recoverable.
	{
		const Threading::ThreadHandle cpu_thread =
			Threading::ThreadHandle::GetForCallingThread();
		if (!cpu_thread.SetAffinity(1u << 0))
			Console.Warning("Vita CPU-thread affinity was rejected; using the scheduler default.");
	}
	VitaGS::SetNativePresenterEnabled(!VITASX2_PRODUCT_BOOT_VALIDATION);
	EmuConfig.BaseFilenames.Bios = BIOS_FILE;

	if (!VMManager::Internal::CPUThreadInitialize())
	{
		Error::SetString(&error, "VMManager::Internal::CPUThreadInitialize() failed.");
		goto fail;
	}
	cpu_thread_initialized = true;

	{
		VMBootParameters boot;
		std::string boot_path;
		ConfiguredBootKind boot_kind = ConfiguredBootKind::Disc;
		if (workload.enabled)
		{
			boot_path = workload.disc_path;
			boot_kind = ConfiguredBootKind::Disc;
			Console.WriteLn("VitaSX2 diagnostic workload boot disc: %s",
				boot_path.c_str());
		}
		else if (!ReadConfiguredBootPath(&boot_path, &boot_kind, &error))
		{
			goto fail;
		}
		if (boot_kind == ConfiguredBootKind::Bios)
		{
			// Exact PCSX2 Start BIOS contract: MainWindow::
			// onStartBIOSActionTriggered() and FullscreenUI::DoStartBIOS() pass a
			// completely default VMBootParameters. VMManager owns resolving the
			// empty filename to NoDisc and suppressing fast boot; the frontend must
			// not encode either result as an override.
			Console.WriteLn("VitaSX2 Start BIOS (default PCSX2 boot parameters).");
		}
		else if (boot_kind == ConfiguredBootKind::Elf)
		{
			boot.elf_override = boot_path;
			boot.source_type = CDVD_SourceType::NoDisc;
			boot.fast_boot = true;
			Console.WriteLn("VitaSX2 boot ELF: %s", boot_path.c_str());
		}
		else
		{
			boot.filename = boot_path;
			boot.source_type = CDVD_SourceType::Iso;
			boot.fast_boot = true;
			Console.WriteLn("VitaSX2 boot disc: %s", boot_path.c_str());
		}
		if (VMManager::Initialize(boot, &error) != VMBootResult::StartupSuccess)
			goto fail;
	}
	vm_initialized = true;
	{
		const std::string configured_bios_path =
			Path::Combine(EmuFolders::Bios, BIOS_FILE);
		if (BiosPath != configured_bios_path)
		{
			Error::SetStringFmt(&error,
				"PCSX2 loaded BIOS '{}' instead of the configured exact image '{}'.",
				BiosPath, configured_bios_path);
			goto fail;
		}
		// PCSX2's LoadBIOS() also probes for optional .rom1/.rom2 files beside
		// the selected image. This product uses one complete, authenticated
		// 4 MiB retail image, so accepting an unauthenticated companion would
		// make the guest machine differ from both the workload metadata and the
		// receipt even though the primary-image SHA-256 still matched.
		if (BiosRom.size() != Ps2MemSize::Rom)
		{
			Error::SetStringFmt(&error,
				"PCSX2 composed {} BIOS bytes, expected only the authenticated {}-byte image; remove or authenticate adjacent ROM1/ROM2 companions.",
				BiosRom.size(), static_cast<size_t>(Ps2MemSize::Rom));
			goto fail;
		}
		Console.WriteLn(
			"VitaSX2 BIOS loaded: path=%s bytes=%u checksum=%08x description=%s.",
			BiosPath.c_str(), static_cast<u32>(BiosRom.size()), BiosChecksum,
			BiosDescription.c_str());
	}
#if VITASX2_WORKLOAD_REPLAY_CHECKPOINT
	RemoveOutput(workload.terminal_checkpoint_path.c_str());
	RemoveOutput(workload.terminal_done_path.c_str());
	{
		Pcsx2Trace::MachineCheckpointTraceConfig checkpoint;
		checkpoint.output_path = workload.terminal_checkpoint_path;
		checkpoint.max_records = 1;
		checkpoint.after_vsync_frames = workload.terminal_vsync_frames;
		checkpoint.wait_for_elf_entry = true;
		if (!Pcsx2Trace::StartMachineCheckpointTrace(checkpoint, &error))
			goto fail;
		workload_checkpoint_started = true;
	}
#endif
	if (workload.enabled)
	{
		const PortableStateLoadResult load_result =
			SaveState_LoadPortableStateFileForVitaWorkloadReplay(
				workload.state_path.c_str(), &error);
		if (load_result != PortableStateLoadResult::Loaded)
		{
			if (!error.IsValid())
			{
				Error::SetStringFmt(&error,
					"Workload '{}' portable state load failed (result={}).",
					workload.slug, static_cast<u32>(load_result));
			}
			goto fail;
		}

		// The portable payload owns emulated PAD/SIO state, while the product's
		// frame-driven autofire phase and correlated profiler origin are host
		// state. Re-arm both only after every load owner has succeeded.
		InputManager::ResetVitaPadAutoFire();
		InputManager::BeginVitaPadDeterministicReplay();
		VitaGS::NotifyPerformanceWorkloadReplayLoaded();
#if VITASX2_WORKLOAD_REPLAY_CHECKPOINT
		Pcsx2Trace::BeginPortableReplayExternalDeviceAccessWindow();
		workload_external_window = true;
		Pcsx2Trace::NotifyMachineCheckpointElfEntry(cpuRegs.pc);
		VitaSetA32EeTraceLimitStopCondition(
			VitaA32EeTraceLimitStopCondition::MachineCheckpointTrace);
#endif
		Console.WriteLn(
			"VitaSX2 workload loaded: slug=%s disc=%s state=%s card1=%s card2=%s frame=%u ee_pc=%08x iop_pc=%08x ee_cycle=%llu iop_cycle=%llu.",
			workload.slug.c_str(), workload.disc_basename.c_str(),
			workload.state_basename.c_str(), workload.card1_basename.c_str(),
			workload.card2_basename.c_str(), g_FrameCount, cpuRegs.pc, psxRegs.pc,
			static_cast<unsigned long long>(cpuRegs.cycle),
			static_cast<unsigned long long>(psxRegs.cycle));
	}

	if (!NativeProvidersSelected())
	{
		Error::SetString(&error, "The product lifecycle did not select all four A32 providers.");
		goto fail;
	}
	if (VITASX2_PRODUCT_BOOT_VALIDATION &&
		(VMManager::GetDiscSerial() != "SCPS-15097" ||
			VMManager::GetDiscCRC() != 0x877f3436u))
	{
		Error::SetStringFmt(&error,
			"Validation disc identity mismatch (serial='{}', crc={:08x}).",
			VMManager::GetDiscSerial(), VMManager::GetDiscCRC());
		goto fail;
	}

#if VITASX2_PRODUCT_BOOT_VALIDATION
	if (!VitaGS::ValidateCanonicalLocalMemoryRing(&gs_ring_validation, &error))
		goto fail;
	Console.WriteLn(
		"VitaSX2 canonical GS ring validation passed (bytes=%u, hash=%016llx, clut=%u).",
		gs_ring_validation.canonical_bytes,
		static_cast<unsigned long long>(gs_ring_validation.local_hash),
		gs_ring_validation.clut_cases);
#endif

	{
		char initialized[3072];
		const int length = std::snprintf(initialized, sizeof(initialized),
			"status=initialized\nbios=%s\nbios_checksum=%08x\nbios_description=%s\n"
			"serial=%s\nelf=%s\ncrc=%08x\nfpcr=%08x\n"
			"providers=ee-a32,iop-a32,vu0-a32,vu1-a32\ncard0=%u\ncard1=%u\n"
			"workload_replay=%u\nworkload_slug=%s\nworkload_manifest=%s\n"
			"workload_disc=%s\nworkload_state=%s\nworkload_card1=%s\n"
			"workload_card2=%s\nworkload_manifest_version=%u\n"
			"workload_disc_bytes=%llu\nworkload_disc_sha256=%s\n"
			"workload_state_bytes=%llu\nworkload_state_sha256=%s\n"
			"workload_card1_bytes=%llu\nworkload_card1_sha256=%s\n"
			"workload_card2_bytes=%llu\nworkload_card2_sha256=%s\n"
			"workload_input_source=%s\nworkload_input_button=%s\n"
			"workload_input_pressed_frames=%u\n"
			"workload_input_released_frames=%u\n"
			"workload_input_physical=%u\n"
			"workload_terminal_vsync_frames=%u\n"
			"workload_terminal_projection=%s\n"
			"workload_disc_attestation_cached=%u\n"
			"workload_card1_provisioned=%u\n"
			"workload_card2_provisioned=%u\nworkload_frame=%u\nworkload_ee_pc=%08x\n"
			"workload_iop_pc=%08x\n",
			BiosPath.c_str(), BiosChecksum, BiosDescription.c_str(),
			VMManager::GetDiscSerial().c_str(), VMManager::GetDiscELF().c_str(),
			VMManager::GetDiscCRC(),
			static_cast<u32>(FPControlRegister::GetCurrent().bitmask),
			FileMcd_IsPresent(0, 0) != 0 ? 1u : 0u,
			FileMcd_IsPresent(1, 0) != 0 ? 1u : 0u,
			workload.enabled ? 1u : 0u,
			workload.enabled ? workload.slug.c_str() : "none",
			workload.enabled ? "workload.ini" : "none",
			workload.enabled ? workload.disc_basename.c_str() : "none",
			workload.enabled ? workload.state_basename.c_str() : "none",
			workload.enabled ? workload.card1_basename.c_str() : "none",
			workload.enabled ? workload.card2_basename.c_str() : "none",
			workload.enabled ? WORKLOAD_MANIFEST_VERSION : 0u,
			static_cast<unsigned long long>(workload.disc_identity.bytes),
			workload.enabled ? workload.disc_identity.sha256.c_str() : "none",
			static_cast<unsigned long long>(workload.state_identity.bytes),
			workload.enabled ? workload.state_identity.sha256.c_str() : "none",
			static_cast<unsigned long long>(workload.card1_identity.bytes),
			workload.enabled ? workload.card1_identity.sha256.c_str() : "none",
			static_cast<unsigned long long>(workload.card2_identity.bytes),
			workload.enabled ? workload.card2_identity.sha256.c_str() : "none",
			workload.enabled ? "deterministic-vsync-v1" : "none",
			workload.enabled ?
				(workload.input_button == InputManager::VitaPadAutoFireButton::Cross ?
					"Cross" :
					(workload.input_button == InputManager::VitaPadAutoFireButton::Circle ?
						"Circle" : "None")) :
				"none",
			workload.enabled ? workload.input_pressed_frames : 0u,
			workload.enabled ? workload.input_released_frames : 0u,
			0u,
			workload.enabled ? workload.terminal_vsync_frames : 0u,
			workload.enabled ? "machine-checkpoint-v5" : "none",
			workload.enabled && workload.disc_attestation_cached ? 1u : 0u,
			workload.enabled && workload.card1_provisioned ? 1u : 0u,
			workload.enabled && workload.card2_provisioned ? 1u : 0u,
			g_FrameCount, cpuRegs.pc, psxRegs.pc);
		if (length <= 0 || static_cast<size_t>(length) >= sizeof(initialized) ||
			!PublishStatus(initialized_path,
				std::string_view(initialized, static_cast<size_t>(length))))
		{
			Error::SetString(&error, "Failed to publish the initialized product state.");
			goto fail;
		}
	}

	if (VITASX2_PRODUCT_BOOT_VALIDATION)
	{
#if VITASX2_PRODUCT_BOOT_VALIDATION
		VitaResetA32EeSessionFallbackStats();
		Pcsx2Trace::EeTraceConfig trace;
		trace.output_path = VALIDATION_TRACE_PATH;
		trace.max_records = VALIDATION_EE_RECORDS;
		trace.wait_for_elf_entry = true;
		trace.record_elf_entry_state = true;
		if (!Pcsx2Trace::StartEeTrace(trace, &error))
			goto fail;
		trace_started = true;

		Pcsx2Trace::MachineCheckpointTraceConfig checkpoint;
		checkpoint.output_path = VALIDATION_CHECKPOINT_PATH;
		checkpoint.max_records = VALIDATION_CHECKPOINT_RECORDS;
		checkpoint.after_vsync_frames = VALIDATION_AFTER_VSYNC_FRAMES;
		checkpoint.wait_for_elf_entry = true;
		if (!Pcsx2Trace::StartMachineCheckpointTrace(checkpoint, &error))
			goto fail;
		checkpoint_started = true;
		// The ELF-entry owner installs the callback and exact stream handling,
		// leaving the complete BIOS/EELOAD prefix on the normal callable native path.
		// Once that bounded stream fills, the host removes the callback and the
		// same VM continues through the production persistent/direct-linked path.
#endif
	}

	VMManager::SetPaused(false);
	while (true)
	{
		const VMState state = VMManager::GetState();
		if (state == VMState::Running)
		{
			VMManager::Execute();
#if VITASX2_WORKLOAD_REPLAY_CHECKPOINT
			if (!workload_checkpoint_complete &&
				Pcsx2Trace::DidMachineCheckpointTraceHitLimit())
			{
				VitaSetA32EeTraceLimitStopCondition(
					VitaA32EeTraceLimitStopCondition::None);
				if (Pcsx2Trace::GetMachineCheckpointTraceRecordsWritten() != 1 ||
					!Pcsx2Trace::GetMachineCheckpointTraceError().empty())
				{
					Error::SetStringFmt(&error,
						"Workload checkpoint failed (records={}, error='{}').",
						Pcsx2Trace::GetMachineCheckpointTraceRecordsWritten(),
						Pcsx2Trace::GetMachineCheckpointTraceError());
					goto fail;
				}

				const Pcsx2Trace::PortableReplayExternalDeviceAccessCounts accesses =
					Pcsx2Trace::GetPortableReplayExternalDeviceAccessCounts();
				Pcsx2Trace::EndPortableReplayExternalDeviceAccessWindow();
				workload_external_window = false;
				if (!accesses.IsZero())
				{
					Error::SetStringFmt(&error,
						"Workload replay touched unserialized devices (DEV9 r/w/dma/irq={}/{}/{}/{}/{}, FW r/w/irq={}/{}/{}).",
						accesses.dev9_reads, accesses.dev9_writes,
						accesses.dev9_dma, accesses.dev9_irq_scheduled,
						accesses.dev9_irq_delivered, accesses.firewire_reads,
						accesses.firewire_writes, accesses.firewire_irq);
					goto fail;
				}

				// Seal the binary before publishing terminal.done. The runner treats
				// that atomic marker as authority that the checkpoint is complete.
				Pcsx2Trace::StopMachineCheckpointTrace();
				workload_checkpoint_started = false;
				if (!PublishWorkloadTerminal(workload, accesses, &error))
					goto fail;
				workload_checkpoint_complete = true;
				Console.WriteLn(
					"VitaSX2 workload terminal checkpoint sealed: slug=%s frame=%u ee_pc=%08x iop_pc=%08x.",
					workload.slug.c_str(), g_FrameCount, cpuRegs.pc, psxRegs.pc);
				continue;
			}
#endif
#if VITASX2_PRODUCT_BOOT_VALIDATION
			if (!entry_trace_complete && Pcsx2Trace::DidEeTraceHitLimit())
			{
				VitaSetEePreInstructionTraceCallback(nullptr);
				VitaSetEeExactTraceStreams(false);
				Pcsx2Trace::StopEeTrace();
				trace_started = false;
				if (Pcsx2Trace::GetEeTraceRecordsWritten() != VALIDATION_EE_RECORDS ||
					!Pcsx2Trace::GetEeTraceError().empty() ||
					!VMManager::Internal::HasBootedELF())
				{
					Error::SetStringFmt(&error,
						"Boot trace failed (records={}, booted={}, error='{}').",
						Pcsx2Trace::GetEeTraceRecordsWritten(),
						VMManager::Internal::HasBootedELF(),
						Pcsx2Trace::GetEeTraceError());
					goto fail;
				}
				entry_trace_complete = true;
				if (!StartValidationProgress())
				{
					Error::SetString(&error,
						"Failed to publish the synchronized entry progress receipt.");
					goto fail;
				}
				VitaSetA32EeTraceLimitStopCondition(
					VitaA32EeTraceLimitStopCondition::MachineCheckpointTrace);
				Console.WriteLn(
					"VitaSX2 exact entry gate passed; continuing to the later GS/VU milestone.");
				continue;
			}
			if (entry_trace_complete &&
				Pcsx2Trace::DidMachineCheckpointTraceHitLimit())
			{
				if (!SealValidationCheckpointProgress())
				{
					Error::SetString(&error,
						"Failed to seal the synchronized checkpoint progress receipt.");
					goto fail;
				}
				StopValidationProgress();
				VMManager::SetState(VMState::Stopping);
			}
#endif
			continue;
		}
		if (state == VMState::Paused)
		{
			VMManager::IdlePollUpdate();
			Threading::Sleep(10);
			continue;
		}
		break;
	}

#if VITASX2_PRODUCT_BOOT_VALIDATION
	StopValidationProgress();
	if (!entry_trace_complete || !Pcsx2Trace::DidEeTraceHitLimit() ||
		Pcsx2Trace::GetEeTraceRecordsWritten() != VALIDATION_EE_RECORDS ||
		!Pcsx2Trace::GetEeTraceError().empty() ||
		!VMManager::Internal::HasBootedELF())
	{
		Error::SetStringFmt(&error,
			"Boot trace failed (records={}, hit_limit={}, booted={}, error='{}').",
			Pcsx2Trace::GetEeTraceRecordsWritten(), Pcsx2Trace::DidEeTraceHitLimit(),
			VMManager::Internal::HasBootedELF(), Pcsx2Trace::GetEeTraceError());
		goto fail;
	}
	if (s_validation_progress.write_failed)
	{
		Error::SetString(&error,
			"The synchronized frame-progress receipt failed during execution.");
		goto fail;
	}
	if (!Pcsx2Trace::DidMachineCheckpointTraceHitLimit() ||
		Pcsx2Trace::GetMachineCheckpointTraceRecordsWritten() !=
			VALIDATION_CHECKPOINT_RECORDS ||
		!Pcsx2Trace::GetMachineCheckpointTraceError().empty())
	{
		Error::SetStringFmt(&error,
			"Later milestone failed (records={}, hit_limit={}, error='{}').",
			Pcsx2Trace::GetMachineCheckpointTraceRecordsWritten(),
			Pcsx2Trace::DidMachineCheckpointTraceHitLimit(),
			Pcsx2Trace::GetMachineCheckpointTraceError());
		goto fail;
	}
	VitaSetA32EeTraceLimitStopCondition(VitaA32EeTraceLimitStopCondition::None);
	Pcsx2Trace::StopMachineCheckpointTrace();
	checkpoint_started = false;

	{
		std::string evidence;
		if (!NativeMilestoneHasNoFallback(&evidence, gs_ring_validation))
		{
			Error::SetString(&error,
				"The all-native reset-to-later milestone used a fallback or missed its workload.");
			goto fail;
		}
		Console.WriteLn("VitaSX2 all-native reset-to-later SOTC milestone passed.");
		VitaPerformanceTelemetry::ShutdownCpuStageProfiler();
		VMManager::Shutdown(false);
		vm_initialized = false;
		VMManager::Internal::CPUThreadShutdown();
		cpu_thread_initialized = false;
		Log::SetFileOutputLevel(LOGLEVEL_NONE, std::string());
		if (!PublishStatus(VALIDATION_DONE_PATH, evidence))
		{
			Error::SetString(&error, "Failed to publish sealed validation evidence.");
			goto fail;
		}
	}
#else
	Error::SetString(&error, "The VitaSX2 execution loop stopped unexpectedly.");
	goto fail;
#endif

	return ExitProduct(0);

fail:
	VitaSetEePreInstructionTraceCallback(nullptr);
	VitaSetEeExactTraceStreams(false);
	VitaPerformanceTelemetry::ShutdownCpuStageProfiler();
#if VITASX2_WORKLOAD_REPLAY_CHECKPOINT
	VitaSetA32EeTraceLimitStopCondition(VitaA32EeTraceLimitStopCondition::None);
	if (workload_external_window)
		Pcsx2Trace::EndPortableReplayExternalDeviceAccessWindow();
	if (workload_checkpoint_started)
		Pcsx2Trace::StopMachineCheckpointTrace();
#endif
#if VITASX2_PRODUCT_BOOT_VALIDATION
	StopValidationProgress();
	VitaSetA32EeTraceLimitStopCondition(VitaA32EeTraceLimitStopCondition::None);
	if (checkpoint_started)
		Pcsx2Trace::StopMachineCheckpointTrace();
#endif
	if (trace_started)
		Pcsx2Trace::StopEeTrace();
	if (vm_initialized || VMManager::GetState() != VMState::Shutdown)
		VMManager::Shutdown(false);
	if (cpu_thread_initialized)
		VMManager::Internal::CPUThreadShutdown();

	{
		const std::string description = error.IsValid() ?
			error.GetDescription() : "unspecified VitaSX2 startup failure";
		Console.Error("VitaSX2 product failure: %s", description.c_str());
		Log::SetFileOutputLevel(LOGLEVEL_NONE, std::string());
		const std::string marker = "status=failed\nerror=" + description + "\n";
		PublishStatus(failed_path, marker);
		sceClibPrintf("VitaSX2: failure: %s\n", description.c_str());
	}
	return ExitProduct(1);
}
