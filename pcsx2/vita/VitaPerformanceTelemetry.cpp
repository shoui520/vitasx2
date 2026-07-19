// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaPerformanceTelemetry.h"

namespace VitaPerformanceTelemetry
{
	bool g_enabled = true;

	void SetEnabledBeforeVmStart(bool enabled)
	{
		g_enabled = enabled;
	}
}
