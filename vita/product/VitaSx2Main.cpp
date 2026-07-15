// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

// VitaSX2 product entrypoint.
//
// This is intentionally a thin host. PCSX2's VM lifecycle lives in
// pcsx2/vita/VitaVmState.cpp; the product owns only Vita paths, target policy,
// persistent status/log files, and the foreground state loop.

#include "CDVD/CDVD.h"
#include "CDVD/CDVDcommon.h"
#include "Config.h"
#include "DebugTools/EeTrace.h"
#include "Host.h"
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
#include "common/Threading.h"
#include "vita/VitaCore.h"
#include "vita/VitaVuBlockCompiler.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#ifndef VITASX2_PRODUCT_BOOT_VALIDATION
#define VITASX2_PRODUCT_BOOT_VALIDATION 0
#endif

namespace
{
	constexpr const char* NAME = "VitaSX2";
	constexpr const char* DATA_DIR = "ux0:data/vitasx2";
	constexpr const char* BIOS_DIR = "ux0:data/vitasx2/bios";
	constexpr const char* BIOS_FILE = "SCPH-30000 JP 150-010118.BIN";
	constexpr const char* DISC_DIR = "ux0:data/vitasx2/disc";
	constexpr const char* DEFAULT_DISC_PATH =
		"ux0:data/vitasx2/disc/Wander to Kyozou (Japan).iso";
	constexpr const char* BOOT_PATH_CONFIG = "ux0:data/vitasx2/boot-path.txt";
	constexpr const char* PRODUCT_MEMORY_CARD_DIR = "ux0:data/vitasx2/memcards";
	constexpr const char* PRODUCT_LOG_PATH = "ux0:data/vitasx2/vitasx2.log";
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
	constexpr u64 VALIDATION_EE_RECORDS = 128;
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

	void RemoveOutput(const char* path)
	{
		sceIoRemove(path);
		const std::string temporary = std::string(path) + ".tmp";
		sceIoRemove(temporary.c_str());
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
		return FileSystem::EnsureDirectoryExists(memory_card_dir, false, error);
	}

	std::string ReadConfiguredDiscPath()
	{
		if (VITASX2_PRODUCT_BOOT_VALIDATION)
			return DEFAULT_DISC_PATH;

		std::FILE* file = FileSystem::OpenCFile(BOOT_PATH_CONFIG, "rb");
		if (!file)
			return DEFAULT_DISC_PATH;

		char buffer[1024] = {};
		const bool read = std::fgets(buffer, sizeof(buffer), file) != nullptr;
		std::fclose(file);
		if (!read)
			return DEFAULT_DISC_PATH;

		size_t length = std::strlen(buffer);
		while (length > 0 &&
			(buffer[length - 1] == '\r' || buffer[length - 1] == '\n' ||
				buffer[length - 1] == ' ' || buffer[length - 1] == '\t'))
		{
			buffer[--length] = 0;
		}
		const std::string_view path(buffer, length);
		// This unsafe-homebrew product only accepts user-provisioned images in
		// its own data directory. A malformed selector must never broaden reads
		// to the owner's other applications or savedata.
		constexpr std::string_view allowed_prefix = "ux0:data/vitasx2/disc/";
		if (!path.starts_with(allowed_prefix) || path.size() <= allowed_prefix.size())
			return DEFAULT_DISC_PATH;
		const std::string_view filename = path.substr(allowed_prefix.size());
		if (filename == "." || filename == ".." ||
			filename.find('/') != std::string_view::npos ||
			filename.find('\\') != std::string_view::npos ||
			filename.find(':') != std::string_view::npos)
		{
			return DEFAULT_DISC_PATH;
		}
		return std::string(path);
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
		EmuConfig.DEV9.EthEnable = false;
		EmuConfig.DEV9.HddEnable = false;
		if (!VITASX2_PRODUCT_BOOT_VALIDATION)
		{
			// Sony exposes a resume notification, but no documented pre-kill
			// application callback. Until the Vita file-card owner commits each
			// completed PS2 transaction durably, never expose a card whose stdio
			// buffers could be stranded by a LiveArea close or remote destroy.
			for (Pcsx2Config::McdOptions& card : EmuConfig.Mcd)
				card.Enabled = false;
		}

		for (u32 port = 0; port < Pad::NUM_CONTROLLER_PORTS; port++)
			EmuConfig.Pad.Ports[port].Type = Pad::ControllerType::NotConnected;
		if (!VITASX2_PRODUCT_BOOT_VALIDATION)
			EmuConfig.Pad.Ports[0].Type = Pad::ControllerType::DualShock2;

		// The PCSX2 GS state/mailbox is live, but the current Vita target is
		// deliberately headless until the GXM renderer owns Draw()/PCRTC output.
		EmuConfig.GS.Renderer = GSRendererType::Null;
		EmuConfig.GS.SynchronousMTGS = true;
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

	bool NativeProvidersSelected()
	{
		return Cpu == &recCpu && psxCpu == &psxRec &&
			CpuVU0 == static_cast<BaseVUmicroCPU*>(&CpuMicroVU0) &&
			CpuVU1 == static_cast<BaseVUmicroCPU*>(&CpuMicroVU1);
	}

#if VITASX2_PRODUCT_BOOT_VALIDATION
	bool NativeSuffixHasNoFallback(std::string* evidence)
	{
		const VitaA32EeProviderStats ee = VitaGetA32EeProviderStats();
		const VitaA32EeProviderStats ee_session =
			VitaGetA32EeSessionFallbackStats();
		const VitaA32IopProviderStats iop = VitaGetA32IopProviderStats();
		const VitaVU::Vu1ProviderStats vu0 = VitaVU::GetVu0ProviderStats();
		const VitaVU::Vu1ProviderStats vu1 = VitaVU::GetVu1ProviderStats();

		char text[2048];
		const int length = std::snprintf(text, sizeof(text),
			"status=ok\nserial=%s\nelf=%s\ncrc=%08x\nentry=%08x\n"
			"ee_records=%llu\nee_blocks=%u\nee_instructions=%u\nee_interpreter=%u\nee_failed=%u\n"
			"ee_session_interpreter=%u\nee_session_failed=%u\n"
			"ee_session_scan_unsupported=%u\nee_session_scan_boundary=%u\n"
			"ee_session_trace_branch_likely=%u\nee_session_execute_failed=%u\n"
			"ee_session_interpreter_path=%u\n"
			"iop_entries=%llu\niop_interpreter=%u\niop_failed=%u\n"
			"vu0_blocks=%llu\nvu0_pairs=%llu\nvu0_interpreter=%llu\nvu0_failed=%u\n"
			"vu1_blocks=%llu\nvu1_pairs=%llu\nvu1_interpreter=%llu\nvu1_failed=%u\n",
			VMManager::GetDiscSerial().c_str(), VMManager::GetDiscELF().c_str(),
			VMManager::GetDiscCRC(), VMManager::Internal::GetCurrentELFEntryPoint(),
			static_cast<unsigned long long>(Pcsx2Trace::GetEeTraceRecordsWritten()),
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
			static_cast<unsigned long long>(vu1.interpreter_steps), vu1.compile_failures);
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
		return NativeProvidersSelected() && ee.compiled_blocks > 0 &&
			iop.executed_blocks > 0 && ee_only_used_trace_steps &&
			ee_session.failed_blocks == 0 && iop.interpreter_blocks == 0 &&
			iop.failed_blocks == 0 && vu0.interpreter_steps == 0 &&
			vu0.compile_failures == 0 && vu1.interpreter_steps == 0 &&
			vu1.compile_failures == 0;
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
	const char* log_path = VITASX2_PRODUCT_BOOT_VALIDATION ?
		VALIDATION_LOG_PATH : PRODUCT_LOG_PATH;
	const char* initialized_path = VITASX2_PRODUCT_BOOT_VALIDATION ?
		VALIDATION_INITIALIZED_PATH : PRODUCT_INITIALIZED_PATH;
	const char* failed_path = VITASX2_PRODUCT_BOOT_VALIDATION ?
		VALIDATION_FAILED_PATH : PRODUCT_FAILED_PATH;

	if (!EnsureDirectories(&error))
		goto fail;
	Host::Internal::SetBaseSettingsLayer(&s_base_settings);
	Host::Internal::SetSecretsSettingsLayer(&s_secrets_settings);

	RemoveOutput(log_path);
	RemoveOutput(initialized_path);
	RemoveOutput(failed_path);
	if (VITASX2_PRODUCT_BOOT_VALIDATION)
	{
		RemoveOutput(VALIDATION_TRACE_PATH);
		RemoveOutput(VALIDATION_DONE_PATH);
		// This build owns an isolated pair of throwaway cards. Recreate them so
		// the physical boot and x86 oracle begin with the same SIO2 state; normal
		// product cards are never touched.
		sceIoRemove(VALIDATION_CARD_1);
		sceIoRemove(VALIDATION_CARD_2);
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
	ConfigureProductSettings();
	EmuConfig.BaseFilenames.Bios = BIOS_FILE;

	if (!VMManager::Internal::CPUThreadInitialize())
	{
		Error::SetString(&error, "VMManager::Internal::CPUThreadInitialize() failed.");
		goto fail;
	}
	cpu_thread_initialized = true;

	{
		VMBootParameters boot;
		boot.filename = ReadConfiguredDiscPath();
		boot.source_type = CDVD_SourceType::Iso;
		boot.fast_boot = true;
		Console.WriteLn("VitaSX2 boot disc: %s", boot.filename.c_str());
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
		// The ELF-entry owner installs the callback and exact stream handling,
		// leaving the complete BIOS/EELOAD prefix on the normal callable native path.
#endif
	}

	VMManager::SetPaused(false);
	while (true)
	{
		const VMState state = VMManager::GetState();
		if (state == VMState::Running)
		{
			VMManager::Execute();
			if (VITASX2_PRODUCT_BOOT_VALIDATION && Pcsx2Trace::DidEeTraceHitLimit())
				VMManager::SetState(VMState::Stopping);
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

	if (!VITASX2_PRODUCT_BOOT_VALIDATION)
	{
		Error::SetString(&error, "The VitaSX2 execution loop stopped unexpectedly.");
		goto fail;
	}

	VitaSetEePreInstructionTraceCallback(nullptr);
	VitaSetEeExactTraceStreams(false);
	Pcsx2Trace::StopEeTrace();
	trace_started = false;
	if (!Pcsx2Trace::DidEeTraceHitLimit() ||
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

	{
#if VITASX2_PRODUCT_BOOT_VALIDATION
		std::string evidence;
		if (!NativeSuffixHasNoFallback(&evidence))
		{
			Error::SetString(&error, "The all-native ELF-entry suffix used a fallback.");
			goto fail;
		}
		Console.WriteLn("VitaSX2 all-native reset-to-entry validation passed.");
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
#else
		Error::SetString(&error,
			"The non-validation VitaSX2 execution loop stopped unexpectedly.");
		goto fail;
#endif
	}

	return ExitProduct(0);

fail:
	VitaSetEePreInstructionTraceCallback(nullptr);
	VitaSetEeExactTraceStreams(false);
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
