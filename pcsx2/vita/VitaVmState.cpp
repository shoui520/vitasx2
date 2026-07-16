// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "CDVD/CDVD.h"
#include "CDVD/CDVDcommon.h"
#include "Config.h"
#include "DEV9/DEV9.h"
#if !defined(VITASX2_VITA) || defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
#include "DebugTools/CoreEventTrace.h"
#include "DebugTools/MachineCheckpointTrace.h"
#endif
#include "DebugTools/EeTrace.h"
#include "DebugTools/GsTrace.h"
#include "DebugTools/IopTrace.h"
#include "DebugTools/IpuTrace.h"
#include "DebugTools/MemTrace.h"
#include "DebugTools/SifTrace.h"
#include "DebugTools/Spu2Trace.h"
#include "DebugTools/VifTrace.h"
#include "DebugTools/VuTrace.h"
#include "Elfheader.h"
#include "FW.h"
#include "Hw.h"
#include "Input/InputManager.h"
#include "IopBios.h"
#include "Memory.h"
#include "MTGS.h"
#include "MTVU.h"
#include "R3000A.h"
#include "R5900.h"
#include "SaveState.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Sio.h"
#include "SIO/Sio0.h"
#include "SIO/Sio2.h"
#include "SPU2/spu2.h"
#include "USB/USB.h"
#include "VMManager.h"
#include "VUmicro.h"
#include "Vif_Dynarec.h"
#include "vita/VitaCore.h"
#include "vtlb.h"
#include "ps2/BiosTools.h"

#include "common/Error.h"
#include "common/FPControl.h"
#include "common/FileSystem.h"
#include "common/Console.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include <utility>

#if defined(VITASX2_PRODUCT_BOOT_VALIDATION)
static VitaVSyncProgressCallback s_vsync_progress_callback = nullptr;
#endif

namespace VMManager
{
	static VMState s_state = VMState::Shutdown;
	static std::string s_elf_override;
	static std::string s_elf_path;
	static std::string s_disc_serial;
	static std::string s_disc_elf;
	static std::string s_disc_version;
	static const std::string s_empty_string;
	static const std::vector<u32> s_empty_processors;
	static u32 s_current_crc = 0;
	static u32 s_disc_crc = 0;
	static u32 s_elf_entry_point = 0xFFFFFFFFu;
	static bool s_elf_executed = false;
	static bool s_fast_boot_requested = false;

	// PCSX2 owner: VMManager.cpp::{CPUThreadInitialize,Initialize,Shutdown,
	// CPUThreadShutdown}.  Vita has one CPU thread and no desktop frontend, but
	// the same subsystem lifetimes still matter: native code caches may not
	// outlive SysMemory, USB is initialized once per CPU-thread lifetime, and a
	// partially-open VM must unwind in strict reverse order.
	struct VitaLifecycleState
	{
		bool cpu_thread_initialized = false;
		bool memory_allocated = false;
		bool providers_reserved = false;
		bool usb_initialized = false;
		bool vtlb_initialized = false;
		bool cdvd_locked = false;
		bool cdvd_open = false;
		bool memory_cards_open = false;
		bool gs_open = false;
		bool spu2_open = false;
		bool pad_initialized = false;
		bool sio2_initialized = false;
		bool sio0_initialized = false;
		bool dev9_initialized = false;
		bool dev9_open = false;
		bool usb_open = false;
		bool fw_open = false;
	};

	static VitaLifecycleState s_lifecycle;

	static void UpdateELFInfo(std::string elf_path)
	{
		Error error;
		ElfObject elfo;
		if (elf_path.empty() || !cdvdLoadElf(&elfo, elf_path, false, &error))
		{
			// PCSX2 owner: VMManager.cpp::UpdateELFInfo(). The first full-boot
			// EELOAD call intentionally has no ELF argument and enters OSDSYS; it
			// clears game identity without reporting a load failure.
			if (!elf_path.empty())
			{
				Console.Error("Vita ELF load info failed for '%s': %s",
					elf_path.c_str(), error.GetDescription().c_str());
			}
			s_elf_path = {};
			s_elf_entry_point = 0xFFFFFFFFu;
			s_current_crc = 0;
			return;
		}

		elfo.LoadHeaders();
		s_current_crc = elfo.GetCRC();
		s_elf_entry_point = elfo.GetEntryPoint();
		s_elf_path = std::move(elf_path);
	}

	static void ClearELFInfo()
	{
		s_current_crc = 0;
		s_elf_executed = false;
		s_elf_path = {};
		s_elf_entry_point = 0xFFFFFFFFu;
	}

	static void ClearDiscInfo()
	{
		s_disc_serial = {};
		s_disc_elf = {};
		s_disc_version = {};
		s_disc_crc = 0;
	}

	static void UpdateDiscInfo()
	{
		// PCSX2 owner: VMManager.cpp::UpdateDiscDetails(). Disc metadata is
		// machine state, not UI decoration: CDVD.cpp::cdvdReadKey() consumes the
		// serial when emulating the MechaCon disc-key command.
		cdvdGetDiscInfo(&s_disc_serial, &s_disc_elf, &s_disc_version, &s_disc_crc, nullptr);
		Console.WriteLn("Vita disc metadata: serial=%s elf=%s version=%s crc=%08x",
			s_disc_serial.c_str(), s_disc_elf.c_str(), s_disc_version.c_str(), s_disc_crc);
	}

	static bool HasBootedELFState()
	{
		return s_current_crc != 0 && s_elf_executed;
	}

	static void CloseVMSubsystems(bool save_nvram)
	{
		// Mirrors VMManager.cpp::Shutdown() and the failure guards in
		// VMManager.cpp::Initialize().  Each flag is set only after its owner has
		// completed, so this is also safe for initialization failures.
		if (s_lifecycle.fw_open)
		{
			FWclose();
			s_lifecycle.fw_open = false;
		}
		if (s_lifecycle.usb_open)
		{
			USBclose();
			s_lifecycle.usb_open = false;
		}
		if (s_lifecycle.dev9_open)
		{
			DEV9close();
			s_lifecycle.dev9_open = false;
		}
		if (s_lifecycle.dev9_initialized)
		{
			DEV9shutdown();
			s_lifecycle.dev9_initialized = false;
		}
		if (s_lifecycle.sio0_initialized)
		{
			g_Sio0.Shutdown();
			s_lifecycle.sio0_initialized = false;
		}
		if (s_lifecycle.sio2_initialized)
		{
			g_Sio2.Shutdown();
			s_lifecycle.sio2_initialized = false;
		}
		if (s_lifecycle.pad_initialized)
		{
			Pad::Shutdown();
			s_lifecycle.pad_initialized = false;
		}
		if (s_lifecycle.spu2_open)
		{
			SPU2::Close();
			s_lifecycle.spu2_open = false;
		}
		if (s_lifecycle.gs_open)
		{
			MTGS::WaitForClose();
			s_lifecycle.gs_open = false;
		}
		if (s_lifecycle.memory_cards_open)
		{
			FileMcd_EmuClose();
			s_lifecycle.memory_cards_open = false;
		}
		if (s_lifecycle.cdvd_open)
		{
			DoCDVDclose();
			s_lifecycle.cdvd_open = false;
			if (save_nvram)
				cdvdSaveNVRAM();
		}

		CDVDsys_ClearFiles();
		Hle_ClearHostRoot();
		if (s_lifecycle.cdvd_locked)
		{
			cdvdUnlock();
			s_lifecycle.cdvd_locked = false;
		}
	}

	static void ShutdownMachineServices()
	{
		if (!s_lifecycle.vtlb_initialized)
			return;

		// PCSX2 owner: VMManager.cpp::Shutdown(). These services become live in
		// SysMemory::Reset()/cpuReset(), before device opens complete, so the
		// partial-initialization path must retire them independently of VMState.
		R3000A::ioman::reset();
		MemcardBusy::ClearBusy();
		vtlb_Shutdown();
		s_lifecycle.vtlb_initialized = false;
	}

	bool Internal::CPUThreadInitialize()
	{
		if (s_lifecycle.cpu_thread_initialized)
			return true;

		// PCSX2 initializes the host FP environment before allocating the VM.
		// Initialize() installs the configured EE FPCR after provider selection.
		FPControlRegister::SetCurrent(FPControlRegister::GetDefault());
		if (!SysMemory::Allocate())
		{
			Console.Error("Vita VM lifecycle failed to allocate PS2 memory.");
			return false;
		}
		s_lifecycle.memory_allocated = true;

		// PCSX2 owner: VMManager.cpp::InitializeCPUProviders().  All four Vita
		// Reserve() implementations are allocation-free today, but retaining the
		// owner boundary keeps future cache reservations before VM reset.
		recCpu.Reserve();
		psxRec.Reserve();
		CpuMicroVU0.Reserve();
		CpuMicroVU1.Reserve();
		s_lifecycle.providers_reserved = true;
		VifUnpackSSE_Init();

		USBinit();
		s_lifecycle.usb_initialized = true;
		s_lifecycle.cpu_thread_initialized = true;
		return true;
	}

	void Internal::CPUThreadShutdown()
	{
		if (!s_lifecycle.cpu_thread_initialized)
			return;

		if (s_state != VMState::Shutdown)
			Shutdown(false);
		ShutdownMachineServices();

		InputManager::CloseSources();
		if (s_lifecycle.usb_initialized)
		{
			USBshutdown();
			s_lifecycle.usb_initialized = false;
		}
		if (s_lifecycle.providers_reserved)
		{
			// The validated standalone all-native route uses this same order: VU1,
			// VU0, IOP, EE, then SysMemory.  It prevents any provider-private
			// translation from retaining a pointer into released guest memory.
			if (newVifDynaRec)
			{
				dVifRelease(1);
				dVifRelease(0);
			}
			CpuMicroVU1.Shutdown();
			CpuMicroVU0.Shutdown();
			psxRec.Shutdown();
			recCpu.Shutdown();
			s_lifecycle.providers_reserved = false;
		}
		MTGS::ShutdownThread();
		if (s_lifecycle.memory_allocated)
		{
			SysMemory::Release();
			s_lifecycle.memory_allocated = false;
		}

		FPControlRegister::SetCurrent(FPControlRegister::GetDefault());
		s_lifecycle.cpu_thread_initialized = false;
	}

	VMBootResult Initialize(const VMBootParameters& boot_params, Error* error)
	{
		if (!s_lifecycle.cpu_thread_initialized || !s_lifecycle.memory_allocated)
		{
			Error::SetString(error,
				"The Vita CPU-thread lifecycle must be initialized before booting a VM.");
			return VMBootResult::StartupFailure;
		}
		if (s_state != VMState::Shutdown)
		{
			Error::SetString(error, "The virtual machine is already running.");
			return VMBootResult::StartupFailure;
		}
		const bool booting_iso = boot_params.source_type.has_value() &&
			boot_params.source_type.value() == CDVD_SourceType::Iso &&
			!boot_params.filename.empty() && boot_params.elf_override.empty();
		const bool no_disc_source = !boot_params.source_type.has_value() ||
			boot_params.source_type.value() == CDVD_SourceType::NoDisc;
		const bool booting_elf = !boot_params.elf_override.empty() &&
			boot_params.filename.empty() &&
			no_disc_source;
		const bool booting_bios = boot_params.filename.empty() &&
			boot_params.elf_override.empty() && no_disc_source;
		if (!booting_iso && !booting_elf && !booting_bios)
		{
			// Vita-only product policy excludes physical drives and desktop source
			// auto-detection. These are the three PCSX2-owned product routes: an
			// ISO, an ELF override with no disc, or OSDSYS with no disc.
			Error::SetString(error,
				"The Vita product requires an ISO, a direct ELF, or a BIOS-only boot.");
			return VMBootResult::StartupFailure;
		}
		if (!boot_params.save_state.empty() || boot_params.state_index.has_value())
		{
			Error::SetString(error,
				"Frontend savestate boot is not enabled in the Vita product lifecycle.");
			return VMBootResult::StartupFailure;
		}
		const std::string& boot_path = booting_elf ?
			boot_params.elf_override : boot_params.filename;
		if (!booting_bios && !FileSystem::FileExists(boot_path.c_str()))
		{
			Error::SetStringFmt(error, "Requested boot file '{}' does not exist.", boot_path);
			return VMBootResult::StartupFailure;
		}
		if (booting_elf && !IsElfFileName(boot_path))
		{
			Error::SetStringFmt(error, "Requested direct boot file '{}' is not an ELF.", boot_path);
			return VMBootResult::StartupFailure;
		}

		VitaClearVmBootState();
		s_elf_override = booting_elf ? boot_path : std::string();
		s_state = VMState::Initializing;
		if (!cdvdLock(error))
			goto fail;
		s_lifecycle.cdvd_locked = true;

		if (booting_iso)
		{
			CDVDsys_SetFile(CDVD_SourceType::Iso, boot_path);
			CDVDsys_ChangeSource(CDVD_SourceType::Iso);
		}
		else
		{
			CDVDsys_ChangeSource(CDVD_SourceType::NoDisc);
		}

		if (!LoadBIOS())
		{
			Error::SetString(error, "Failed to load the configured PlayStation 2 BIOS.");
			goto fail;
		}
		cdvdLoadNVRAM();
		if (!DoCDVDopen(error))
			goto fail;
		s_lifecycle.cdvd_open = true;

		// PCSX2 owner: VMManager.cpp::Initialize(). NoDisc OSDSYS must never
		// enter eeload fast boot even when the global setting is enabled.
		s_fast_boot_requested = (booting_elf || boot_params.fast_boot.value_or(
			static_cast<bool>(EmuConfig.EnableFastBoot))) && !booting_bios;
		if (booting_elf)
		{
			// PCSX2 owner: VMManager::UpdateDiscDetails(). With an ELF override and
			// no disc, the filename supplies the session identity and its own CRC.
			s_disc_serial = Path::GetFileTitle(boot_path);
			s_disc_elf = {};
			s_disc_version = {};
			s_disc_crc = cdvdGetElfCRC(boot_path);
			Console.WriteLn("Vita direct ELF: title=%s crc=%08x path=%s",
				s_disc_serial.c_str(), s_disc_crc, boot_path.c_str());
		}
		else if (booting_iso)
		{
			UpdateDiscInfo();
		}
		else
		{
			// PCSX2 owner: VMManager.cpp::UpdateDiscDetails(). A no-disc OSDSYS
			// session uses the loaded BIOS identity for cards and diagnostics.
			ClearDiscInfo();
			s_disc_serial = BiosSerial;
			Console.WriteLn("Vita BIOS session: serial=%s zone=%s",
				s_disc_serial.c_str(), BiosZone.c_str());
		}
		// A serial is optional for bootable homebrew discs. PCSX2 uses the ELF
		// filename as a title fallback and an empty memory-card filter.
		if (booting_iso && s_disc_elf.empty())
		{
			Error::SetString(error,
				"The mounted image does not contain a valid PS2 boot executable.");
			goto fail;
		}
		// PCSX2 owner: VMManager.cpp::UpdateDiscDetails() reopens cards only
		// after the disc serial is known. FileMcd_Reopen() also publishes that
		// serial to SIO2 for per-game card filtering/eject behavior.
		FileMcd_Reopen(s_disc_serial);
		s_lifecycle.memory_cards_open = true;
		if (booting_bios)
			Hle_ClearHostRoot();
		else
			Hle_SetHostRoot(boot_path.c_str());

		// PCSX2 owner: UpdateCPUImplementations(), ClearCPUExecutionCaches(),
		// FPCR installation, memory-handler binding, and cold reset in
		// VMManager.cpp::Initialize().  Provider flags and pointers are selected
		// together so generic EE/IOP/VU code observes one coherent contract.
		VitaSelectConfiguredCpuProviders();
		mmap_ResetBlockTracking();
		memSetExtraMemMode(EmuConfig.Cpu.ExtraMemory);
		Internal::ClearCPUExecutionCaches();
		// PCSX2 owner: VMManager.cpp::ApplyGameFixes(). BIOS/EELOAD always
		// requires InstantDMAHack, including a NoDisc OSDSYS boot; the ELF-entry
		// seam below clears it before ordinary game execution becomes observable.
		EmuConfig.Gamefixes.InstantDMAHack = true;
		EmuConfig.GS.ManualUserHacks = false;
		FPControlRegister::SetCurrent(EmuConfig.Cpu.FPUFPCR);
		if (FPControlRegister::GetCurrent() != EmuConfig.Cpu.FPUFPCR)
		{
			Error::SetString(error, "The Cortex-A9 FPCR does not match the configured EE contract.");
			goto fail;
		}
		memBindConditionalHandlers();
		SysMemory::Reset();
		s_lifecycle.vtlb_initialized = true;
		cpuReset();

		if (!MTGS::WaitForOpen())
		{
			Error::SetString(error, "Failed to initialize the GS state owner.");
			goto fail;
		}
		s_lifecycle.gs_open = true;
		if (!SPU2::Open())
		{
			Error::SetString(error, "Failed to initialize SPU2.");
			goto fail;
		}
		s_lifecycle.spu2_open = true;
		if (!Pad::Initialize())
		{
			Error::SetString(error, "Failed to initialize PAD.");
			goto fail;
		}
		s_lifecycle.pad_initialized = true;
		if (!g_Sio2.Initialize())
		{
			Error::SetString(error, "Failed to initialize SIO2.");
			goto fail;
		}
		s_lifecycle.sio2_initialized = true;
		if (!g_Sio0.Initialize())
		{
			Error::SetString(error, "Failed to initialize SIO0.");
			goto fail;
		}
		s_lifecycle.sio0_initialized = true;
		if (DEV9init() != 0)
		{
			Error::SetString(error, "Failed to initialize DEV9.");
			goto fail;
		}
		s_lifecycle.dev9_initialized = true;
		if (DEV9open() != 0)
		{
			Error::SetString(error, "Failed to open DEV9.");
			goto fail;
		}
		s_lifecycle.dev9_open = true;
		if (!USBopen())
		{
			Error::SetString(error, "Failed to open USB.");
			goto fail;
		}
		s_lifecycle.usb_open = true;
		if (FWopen() != 0)
		{
			Error::SetString(error, "Failed to open FireWire.");
			goto fail;
		}
		s_lifecycle.fw_open = true;

		hwReset();
		s_state = VMState::Paused;
		SPU2::SetOutputPaused(true);
		return VMBootResult::StartupSuccess;

	fail:
		ShutdownMachineServices();
		CloseVMSubsystems(false);
		ClearELFInfo();
		ClearDiscInfo();
		s_elf_override = {};
		s_fast_boot_requested = false;
		s_state = VMState::Shutdown;
		return VMBootResult::StartupFailure;
	}

	void Shutdown(bool save_resume_state)
	{
		(void)save_resume_state;
		if (s_state == VMState::Shutdown)
			return;

		s_state = VMState::Stopping;
		if (THREAD_VU1 && vu1Thread.IsOpen())
			vu1Thread.WaitVU();
		MTGS::WaitGS(false, false, false);
		FPControlRegister::SetCurrent(FPControlRegister::GetDefault());
		InputManager::PauseVibration();
		ShutdownMachineServices();
		CloseVMSubsystems(true);
		ClearELFInfo();
		ClearDiscInfo();
		s_elf_override = {};
		s_fast_boot_requested = false;
		s_state = VMState::Shutdown;
	}

	void Execute()
	{
		if (s_state != VMState::Running || !Cpu)
			return;
		Cpu->Execute();
	}

	void IdlePollUpdate()
	{
		InputManager::PollSources();
	}

	bool PerformEarlyHardwareChecks(const char** error)
	{
		return true;
	}

	bool IsElfFileName(const std::string_view path)
	{
		// Exact PCSX2 owner: VMManager.cpp::IsElfFileName().
		return StringUtil::EndsWithNoCase(path, ".elf");
	}

	VMState GetState()
	{
		return s_state;
	}

	void SetState(VMState state)
	{
		const VMState old_state = s_state;
		s_state = state;
		if (old_state == VMState::Running && state != VMState::Running &&
			Cpu && Cpu->ExitExecution)
		{
			Cpu->ExitExecution();
		}
		if (s_lifecycle.spu2_open && state != VMState::Stopping &&
			(state == VMState::Paused || old_state == VMState::Paused))
		{
			const bool paused = state == VMState::Paused;
			if (paused)
				InputManager::PauseVibration();
			SPU2::SetOutputPaused(paused);
		}
	}

	bool HasValidVM()
	{
		return s_state == VMState::Running || s_state == VMState::Paused ||
			s_state == VMState::Resetting;
	}

	std::string GetDiscPath()
	{
		return CDVDsys_GetFile(CDVDsys_GetSourceType());
	}

	std::string GetDiscELF()
	{
		return s_disc_elf;
	}

	std::string GetTitle(bool prefer_en)
	{
		return s_elf_path;
	}

	u32 GetDiscCRC()
	{
		return s_disc_crc;
	}

	std::string GetDiscVersion()
	{
		return s_disc_version;
	}

	u32 GetCurrentCRC()
	{
		return s_current_crc;
	}

	const std::string& GetCurrentELF()
	{
		return s_elf_path;
	}

	void SetPaused(bool paused)
	{
		if (!HasValidVM())
			return;
		SetState(paused ? VMState::Paused : VMState::Running);
	}

	float GetTargetSpeed()
	{
		return 1.0f;
	}

	bool IsTargetSpeedAdjustedToHost()
	{
		return false;
	}

	float GetFrameRate()
	{
		return 0.0f;
	}

	std::string GetDiscSerial()
	{
		return s_disc_serial;
	}

	void UpdateTargetSpeed()
	{
	}

	void UpdateDiscordPresence(bool update_session_time)
	{
	}

	GSVSyncMode GetEffectiveVSyncMode()
	{
		// PCSX2 owner: VMManager.cpp::GetEffectiveVSyncMode(). Vita has one
		// fixed display queue and no desktop host-refresh synchronizer; retain
		// the user-visible enable/disable decision and use its blocking mode.
		return EmuConfig.GS.VsyncEnable ? GSVSyncMode::FIFO : GSVSyncMode::Disabled;
	}

	bool ShouldAllowPresentThrottle()
	{
		// PCSX2 only enables this while running away from normal target speed.
		// Vita's product VM currently exposes normal-speed execution only.
		return false;
	}

	namespace Internal
	{
		bool IsFastBootInProgress()
		{
			return s_fast_boot_requested && !HasBootedELFState();
		}

		bool WasFastBooted()
		{
			return s_fast_boot_requested;
		}

		void DisableFastBoot()
		{
			s_fast_boot_requested = false;
		}

		bool HasBootedELF()
		{
			return HasBootedELFState();
		}

		u32 GetCurrentELFEntryPoint()
		{
			return s_elf_entry_point;
		}

		void FrameRateChanged()
		{
		}

		void Throttle()
		{
		}

		void ClearCPUExecutionCaches()
		{
			Cpu->Reset();
			psxCpu->Reset();
			CpuVU0->Reset();
			CpuVU1->Reset();
			// Exact PCSX2 owner: VMManager::Internal::ClearCPUExecutionCaches().
			// The Vita VIF dynarec owns persistent unpack state independently of
			// the VU providers, so reset both channels at the same lifecycle seam.
			if constexpr (newVifDynaRec)
			{
				dVifReset(0);
				dVifReset(1);
			}
		}

		const std::vector<u32>& GetSoftwareRendererProcessorList()
		{
			return s_empty_processors;
		}

		const std::string& GetELFOverride()
		{
			return s_elf_override;
		}

		bool IsExecutionInterrupted()
		{
			return s_state != VMState::Running;
		}

		void ELFLoadingOnCPUThread(std::string elf_path)
		{
			UpdateELFInfo(std::move(elf_path));
			s_elf_executed = false;
		}

		void EntryPointCompilingOnCPUThread()
		{
			if (s_elf_executed)
				return;

			// PCSX2 owner: Interpreter.cpp::intExecute() notifies every
			// trace domain immediately before EntryPointCompilingOnCPUThread().
			// Vita's VM shim owns that edge for both interpreter and A32 EE/IOP.
			Pcsx2Trace::NotifyEeElfEntry(s_elf_entry_point);
#if defined(VITASX2_PRODUCT_BOOT_VALIDATION)
			// Do not install the callback before this owner boundary: its presence
			// routes callable blocks through validation prerecording. BIOS/EELOAD
			// therefore retains the normal callable native path, and the exact
			// bounded stream begins on the first game instruction.
			VitaSetEeExactTraceStreams(true);
			VitaSetEePreInstructionTraceCallback(Pcsx2Trace::RecordEePreInstruction);
#endif
#if !defined(VITASX2_VITA) || defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
			Pcsx2Trace::NotifyCoreEventElfEntry(s_elf_entry_point);
			Pcsx2Trace::NotifyMachineCheckpointElfEntry(s_elf_entry_point);
#endif
			Pcsx2Trace::NotifyMemElfEntry(s_elf_entry_point);
			Pcsx2Trace::NotifyGsElfEntry(s_elf_entry_point);
			Pcsx2Trace::NotifyIopElfEntry(s_elf_entry_point);
			Pcsx2Trace::NotifyIpuElfEntry(s_elf_entry_point);
			Pcsx2Trace::NotifySifElfEntry(s_elf_entry_point);
			Pcsx2Trace::NotifySpu2ElfEntry(s_elf_entry_point);
			Pcsx2Trace::NotifyVifElfEntry(s_elf_entry_point);
			Pcsx2Trace::NotifyVuElfEntry(s_elf_entry_point);
			s_elf_executed = true;
			// Mirrors VMManager.cpp::EntryPointCompilingOnCPUThread() -> HandleELFChange(true):
			// the BIOS/EELOAD-only InstantDMAHack from ApplyGameFixes() is cleared once the game ELF owns execution.
			EmuConfig.Gamefixes.InstantDMAHack = false;
			// Any configuration-driven card reopen happened before the game could
			// observe the card. Keep PCSX2's entry-point eject cancellation even
			// while the Vita product has no patch/settings frontend.
			FileMcd_CancelEject();
			mmap_ResetBlockTracking();
			ClearCPUExecutionCaches();
			memBindConditionalHandlers();
			ClearCPUExecutionCaches();
			// PCSX2 owner: VMManager.cpp records the final pre-first-instruction
			// boundary only after boot patches/settings and cache publication.
			Pcsx2Trace::RecordEeElfEntryState(s_elf_entry_point);
		}

		void VSyncOnCPUThread()
		{
#if defined(VITASX2_PRODUCT_BOOT_VALIDATION)
			if (s_vsync_progress_callback)
				s_vsync_progress_callback();
#endif
		}

		void PollInputOnCPUThread()
		{
			// PCSX2 owner: VMManager.cpp::Internal::PollInputOnCPUThread(),
			// called by Counters.cpp::VSyncStart() after the frame push. Keep
			// Vita controller sampling on that emulated-frame boundary so SIO2
			// observes one coherent snapshot for the following frame.
			InputManager::PollSources();
		}
	} // namespace Internal

} // namespace VMManager

#if defined(VITASX2_PRODUCT_BOOT_VALIDATION)
void VitaSetVSyncProgressCallback(VitaVSyncProgressCallback callback)
{
	s_vsync_progress_callback = callback;
}
#endif

void VitaClearVmBootState()
{
	VMManager::SetState(VMState::Shutdown);
	VMManager::ClearELFInfo();
	VMManager::ClearDiscInfo();
	VMManager::s_elf_override = {};
	VMManager::s_fast_boot_requested = false;
}

void VitaSetFastBootElfOverride(const char* elf_path)
{
	VMManager::ClearELFInfo();
	VMManager::ClearDiscInfo();
	VMManager::s_elf_override = elf_path ? std::string(elf_path) : std::string();
	VMManager::s_fast_boot_requested = !VMManager::s_elf_override.empty();
}

void VitaSetFastBootDisc()
{
	VMManager::ClearELFInfo();
	VMManager::UpdateDiscInfo();
	VMManager::s_elf_override = {};
	VMManager::s_fast_boot_requested = true;
}

bool SaveStateBase::vmFreeze()
{
	// PCSX2 owner: VMManager.cpp::SaveStateBase::vmFreeze(). Keep the ELF
	// identity in the internal-structures entry; it determines whether the
	// restored PC is BIOS/EELOAD or game code and therefore whether the
	// pre-entry InstantDMA contract is still active.
	const u32 previous_crc = VMManager::s_current_crc;
	const std::string previous_elf = VMManager::s_elf_path;
	const bool previous_elf_executed = VMManager::s_elf_executed;
	Freeze(VMManager::s_current_crc);
	FreezeString(VMManager::s_elf_path);
	Freeze(VMManager::s_elf_executed);

	if (IsLoading())
	{
		if (IsPortableReplay())
		{
			const u32 replay_crc = VMManager::s_current_crc;
			const std::string replay_elf = VMManager::s_elf_path;
			const bool replay_elf_executed = VMManager::s_elf_executed;
			if (replay_elf.empty())
			{
				if (replay_crc != 0 || replay_elf_executed)
				{
					Console.Error("Portable replay contains an invalid empty ELF identity.");
					m_error = true;
					return false;
				}
				VMManager::ClearELFInfo();
			}
			else
			{
				// Re-read the ELF from the currently mounted disc. Normal PCSX2
				// savestates tolerate a changed image; an oracle replay must not.
				VMManager::UpdateELFInfo(replay_elf);
				if (VMManager::s_elf_path != replay_elf ||
					VMManager::s_current_crc != replay_crc)
				{
					Console.Error("Portable replay ELF identity does not match the mounted disc.");
					m_error = true;
					return false;
				}
				VMManager::s_elf_executed = replay_elf_executed;
			}

			EmuConfig.Gamefixes.InstantDMAHack = !VMManager::s_elf_executed;
			return IsOkay();
		}

		if (VMManager::s_elf_path != previous_elf)
		{
			if (VMManager::s_elf_path.empty())
			{
				if (VMManager::s_elf_executed)
					Console.Error("Loaded VM state marks an empty ELF path as executed.");
				VMManager::ClearELFInfo();
			}
			else
			{
				VMManager::UpdateELFInfo(std::move(VMManager::s_elf_path));
			}
		}

		if (VMManager::s_current_crc != previous_crc ||
			VMManager::s_elf_path != previous_elf ||
			VMManager::s_elf_executed != previous_elf_executed)
		{
			// Vita's pruned VM has no desktop settings/patch layer. This is the
			// machine-visible part of VMManager.cpp::HandleELFChange() retained by
			// EntryPointCompilingOnCPUThread() for the deterministic Vita core.
			EmuConfig.Gamefixes.InstantDMAHack = !VMManager::s_elf_executed;
		}
	}

	return IsOkay();
}
