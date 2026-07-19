// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace VitaPerformanceTelemetry
{
	// Validation executables retain their historical telemetry unless their
	// entrypoint chooses otherwise. The product sets this exactly once before
	// PCSX2 creates the EE, MTVU, and MTGS threads.
	extern bool g_enabled;

	inline bool IsEnabled()
	{
		return g_enabled;
	}

	void SetEnabledBeforeVmStart(bool enabled);
}
