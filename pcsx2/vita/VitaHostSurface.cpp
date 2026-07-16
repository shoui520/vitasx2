// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS.h"
#include "Host.h"
#include "VMManager.h"

#include "common/WindowInfo.h"
#include "common/ProgressCallback.h"

#include <cstdarg>
#include <cstring>

#if defined(VITASX2_QEMU_VALIDATION)
#include <cstdio>
#else
#include <psp2/kernel/clib.h>
#endif

namespace
{
	void HostPrint(const char* format, ...)
	{
		std::va_list args;
		va_start(args, format);
#if defined(VITASX2_QEMU_VALIDATION)
		std::vprintf(format, args);
#else
		sceClibVprintf(format, args);
#endif
		va_end(args);
	}

	void PrintHostMessage(const char* kind, const std::string_view title, const std::string_view message)
	{
		HostPrint("Host %s: %.*s: %.*s\n", kind,
			static_cast<int>(title.size()), title.data(),
			static_cast<int>(message.size()), message.data());
	}
} // namespace

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	return msg ? std::string(msg) : std::string();
}

void Host::AddOSDMessage(std::string message, float duration)
{
	HostPrint("Host OSD: %s\n", message.c_str());
}

void Host::AddKeyedOSDMessage(std::string key, std::string message, float duration)
{
	HostPrint("Host OSD[%s]: %s\n", key.c_str(), message.c_str());
}

void Host::AddIconOSDMessage(std::string key, const char* icon, const std::string_view message, float duration)
{
	HostPrint("Host OSD[%s]: %.*s\n", key.c_str(), static_cast<int>(message.size()), message.data());
}

void Host::RemoveKeyedOSDMessage(std::string key)
{
}

void Host::ClearOSDMessages()
{
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	PrintHostMessage("info", title, message);
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	PrintHostMessage("error", title, message);
}

bool Host::InBatchMode()
{
	return true;
}

bool Host::InNoGUIMode()
{
	return true;
}

void Host::OpenURL(const std::string_view url)
{
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	return false;
}

std::string Host::GetTextFromClipboard()
{
	return {};
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	return false;
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

void Host::RunOnCPUThread(std::function<void()> function, bool block)
{
	if (function)
		function();
}

void Host::RunOnGSThread(std::function<void()> function)
{
	if (function)
		function();
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
}

void Host::CancelGameListRefresh()
{
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
}

void Host::CommitBaseSettingChanges()
{
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return nullptr;
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	return lhs.compare(rhs);
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	WindowInfo wi;
	wi.type = WindowInfo::Type::Surfaceless;
	wi.surface_width = 960;
	wi.surface_height = 544;
	wi.surface_scale = 1.0f;
	return wi;
}

void Host::BeginPresentFrame()
{
}

void Host::ReleaseRenderWindow()
{
}

bool Host::IsFullscreen()
{
	return true;
}

void Host::SetFullscreen(bool enabled)
{
}

void Host::OnCaptureStarted(const std::string& filename)
{
}

void Host::OnCaptureStopped()
{
}

void Host::OnPerformanceMetricsUpdated()
{
	// The fixed Vita frontend has no desktop status widgets to invalidate. The
	// metrics themselves are still updated by the GS worker and consumed by the
	// product/runtime diagnostics.
}

s32 Host::Internal::GetTranslatedStringImpl(const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;

	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}
