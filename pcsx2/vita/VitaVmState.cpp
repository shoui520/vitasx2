// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VMManager.h"

namespace VMManager
{
	static VMState s_state = VMState::Shutdown;
	static const std::string s_empty_string;
	static const std::vector<u32> s_empty_processors;

	bool PerformEarlyHardwareChecks(const char** error)
	{
		return true;
	}

	VMState GetState()
	{
		return s_state;
	}

	void SetState(VMState state)
	{
		s_state = state;
	}

	bool HasValidVM()
	{
		return s_state != VMState::Shutdown;
	}

	void SetPaused(bool paused)
	{
		s_state = paused ? VMState::Paused : VMState::Running;
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
		return {};
	}

	void UpdateTargetSpeed()
	{
	}

	void UpdateDiscordPresence(bool update_session_time)
	{
	}

	namespace Internal
	{
		bool IsFastBootInProgress()
		{
			return false;
		}

		bool WasFastBooted()
		{
			return false;
		}

		void DisableFastBoot()
		{
		}

		bool HasBootedELF()
		{
			return false;
		}

		u32 GetCurrentELFEntryPoint()
		{
			return 0;
		}

		void FrameRateChanged()
		{
		}

		void Throttle()
		{
		}

		void ClearCPUExecutionCaches()
		{
		}

		const std::vector<u32>& GetSoftwareRendererProcessorList()
		{
			return s_empty_processors;
		}

		const std::string& GetELFOverride()
		{
			return s_empty_string;
		}

		bool IsExecutionInterrupted()
		{
			return false;
		}

		void ELFLoadingOnCPUThread(std::string elf_path)
		{
		}

		void EntryPointCompilingOnCPUThread()
		{
		}

		void VSyncOnCPUThread()
		{
		}

		void PollInputOnCPUThread()
		{
		}
	} // namespace Internal
} // namespace VMManager
