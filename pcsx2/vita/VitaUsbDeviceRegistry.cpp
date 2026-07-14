// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "USB/deviceproxy.h"

RegisterDevice* RegisterDevice::registerDevice = nullptr;

DeviceProxy::~DeviceProxy() = default;

std::span<const char*> DeviceProxy::SubTypes() const
{
	return {};
}

std::span<const InputBindingInfo> DeviceProxy::Bindings(u32 subtype) const
{
	return {};
}

std::span<const SettingInfo> DeviceProxy::Settings(u32 subtype) const
{
	return {};
}

float DeviceProxy::GetBindingValue(const USBDevice* dev, u32 bind) const
{
	return 0.0f;
}

void DeviceProxy::SetBindingValue(USBDevice* dev, u32 bind, float value) const
{
}

bool DeviceProxy::Freeze(USBDevice* dev, StateWrapper& sw) const
{
	return false;
}

void DeviceProxy::UpdateSettings(USBDevice* dev, SettingsInterface& si) const
{
}

void DeviceProxy::InputDeviceConnected(USBDevice* dev, const std::string_view identifier) const
{
}

void DeviceProxy::InputDeviceDisconnected(USBDevice* dev, const std::string_view identifier) const
{
}

void RegisterDevice::Register()
{
	// PCSX2 owner: USB/deviceproxy.cpp::RegisterDevice::Register(). Keep the
	// registry lifecycle and the real OHCI controller while exposing no host
	// peripheral classes on the bounded Vita core route.
	(void)RegisterDevice::instance();
}

void RegisterDevice::Unregister()
{
	registerDeviceMap.clear();
	delete registerDevice;
	registerDevice = nullptr;
}
