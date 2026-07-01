// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Vita time/memory/OS glue.

#include "common/HostSys.h"
#include "common/Threading.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

u64 GetTickFrequency()
{
	// sceKernelGetProcessTimeWide() is a monotonic microsecond clock.
	return 1000000;
}

u64 GetCPUTicks()
{
	return sceKernelGetProcessTimeWide();
}

u64 GetPhysicalMemory()
{
	// Retail app LPDDR budget (256 MiB base allocation for a standard app).
	return 256ull * 1024 * 1024;
}

u64 GetAvailablePhysicalMemory()
{
	SceKernelFreeMemorySizeInfo info;
	info.size = sizeof(info);
	if (sceKernelGetFreeMemorySize(&info) < 0)
		return 0;
	return static_cast<u64>(info.size_user);
}

std::string GetOSVersionString()
{
	return "PlayStation Vita";
}

bool Common::InhibitScreensaver(bool inhibit)
{
	// Auto-suspend suppression on the Vita is a periodic power tick, not a
	// one-shot; the frame loop owns that. Nothing to do here.
	return true;
}

bool Common::PlaySoundAsync(const char* path)
{
	return false;
}

void Common::SetMousePosition(int x, int y)
{
}

bool Common::AttachMousePositionCb(std::function<void(int, int)> cb)
{
	return false;
}

void Common::DetachMousePositionCb()
{
}

void Threading::Sleep(int ms)
{
	sceKernelDelayThread(static_cast<SceUInt>(ms) * 1000);
}

void Threading::SleepUntil(u64 ticks)
{
	const u64 now = GetCPUTicks();
	if (ticks <= now)
		return;
	sceKernelDelayThread(static_cast<SceUInt>(ticks - now));
}
