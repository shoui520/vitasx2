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

#include "CDVD/CDVD.h"
#include "CDVD/CDVDcommon.h"
#include "Config.h"
#include "Counters.h"
#include "DebugTools/EeTrace.h"
#if VITASX2_PRODUCT_BOOT_VALIDATION
#include "DebugTools/MachineCheckpointTrace.h"
#endif
#include "Host.h"
#include "INISettingsInterface.h"
#include "Input/InputManager.h"
#include "R3000A.h"
#include "R5900.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "SIO/Pad/Pad.h"
#include "VMManager.h"
#include "VUmicro.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/FPControl.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/StringUtil.h"
#include "common/Threading.h"
#include "vita/VitaCore.h"
#include "vita/VitaGsMailbox.h"
#include "vita/VitaPerformanceTelemetry.h"
#include "vita/VitaVuBlockCompiler.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <string>
#include <string_view>

extern "C" unsigned int _newlib_heap_size_user;

namespace
{
	constexpr const char* NAME = "VitaSX2";
	constexpr const char* DATA_DIR = "ux0:data/vitasx2";
	constexpr const char* BIOS_DIR = "ux0:data/vitasx2/bios";
	constexpr const char* BIOS_FILE = "SCPH-30000 JP 150-010118.BIN";
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
	constexpr const char* PRODUCT_INITIALIZED_PATH =
		"ux0:data/vitasx2/vitasx2.initialized";
	constexpr const char* PRODUCT_FAILED_PATH = "ux0:data/vitasx2/vitasx2.failed";

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

	bool PublishHarnessLaunchReceipt(Error* error)
	{
		// tools/run_vita_target.sh writes one unpredictable 64-hex-byte request
		// only after it has closed the foreground application and deployed the
		// selected SELF. Reading it once at process startup and atomically echoing
		// it back proves that this process began after that exact request. An
		// already-running VitaSX2 instance never polls this file and therefore
		// cannot satisfy a later harness invocation accidentally.
		const SceUID fd = sceIoOpen(PRODUCT_LAUNCH_REQUEST_PATH, SCE_O_RDONLY, 0);
		if (fd == PSP2_ERROR_ERRNO_ENOENT)
			return true;
		if (fd < 0)
		{
			Error::SetStringFmt(error, "Failed to open Vita launch request (error={:08x}).",
				static_cast<u32>(fd));
			return false;
		}

		char request[66] = {};
		const SceSSize read = sceIoRead(fd, request, sizeof(request));
		const bool closed = sceIoClose(fd) >= 0;
		bool valid = read == 65 && request[64] == '\n';
		for (u32 i = 0; valid && i < 64; i++)
			valid = (request[i] >= '0' && request[i] <= '9') ||
				(request[i] >= 'a' && request[i] <= 'f');
		if (!closed || !valid)
		{
			Error::SetString(error, "Malformed Vita harness launch request.");
			return false;
		}
		if (!PublishStatus(PRODUCT_LAUNCH_RECEIPT_PATH,
				std::string_view(request, static_cast<size_t>(read))))
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
		Console.WriteLn(
			"VitaSX2 performance telemetry: enabled=%u source=%s.",
			enabled ? 1u : 0u, source);
	}

	void ConfigureProductInputAutomation()
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

		if (!VITASX2_PRODUCT_BOOT_VALIDATION &&
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
				button, pressed_frames, released_frames))
		{
			button = InputManager::VitaPadAutoFireButton::None;
			button_name = "None";
			InputManager::ConfigureVitaPadAutoFire(button, 1, 1);
		}
		Console.WriteLn(
			"VitaSX2 input autofire: button=%s pressed_frames=%u released_frames=%u "
			"start=game-elf source=%s.",
			button_name, pressed_frames, released_frames, source);
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
	ConfigureProductInputAutomation();
	ConfigureProductSettings();
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
		if (!ReadConfiguredBootPath(&boot_path, &boot_kind, &error))
			goto fail;
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
		char initialized[1024];
		const int length = std::snprintf(initialized, sizeof(initialized),
			"status=initialized\nserial=%s\nelf=%s\ncrc=%08x\nfpcr=%08x\n"
			"providers=ee-a32,iop-a32,vu0-a32,vu1-a32\ncard0=%u\ncard1=%u\n",
			VMManager::GetDiscSerial().c_str(), VMManager::GetDiscELF().c_str(),
			VMManager::GetDiscCRC(),
			static_cast<u32>(FPControlRegister::GetCurrent().bitmask),
			FileMcd_IsPresent(0, 0) != 0 ? 1u : 0u,
			FileMcd_IsPresent(1, 0) != 0 ? 1u : 0u);
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
