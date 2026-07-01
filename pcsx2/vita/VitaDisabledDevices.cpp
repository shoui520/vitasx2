// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DEV9/DEV9.h"
#include "FW.h"
#include "USB/USB.h"

#include <array>
#include <cstring>
#include <span>
#include <utility>

struct NetPacket;
class SettingsInterface;
class StateWrapper;

namespace
{
	std::array<s8, 0x10000> s_fwregs = {};
}

s8* fwregs = s_fwregs.data();

void rx_process(NetPacket* pk) {}
bool rx_fifo_can_rx() { return false; }

s32 DEV9init() { return 0; }
void DEV9close() {}
s32 DEV9open() { return 0; }
void DEV9shutdown() {}
void _DEV9irq(int cause, int cycles) {}
int DEV9irqHandler(void) { return 0; }
void DEV9async(u32 cycles) {}
void DEV9runFIFO() {}
void DEV9writeDMA8Mem(u32* pMem, int size) {}
void DEV9readDMA8Mem(u32* pMem, int size)
{
	if (pMem && size > 0)
		std::memset(pMem, 0, static_cast<size_t>(size));
}
u8 DEV9read8(u32 addr) { return 0xff; }
u16 DEV9read16(u32 addr) { return 0xffff; }
u32 DEV9read32(u32 addr) { return 0xffffffffu; }
void DEV9write8(u32 addr, u8 value) {}
void DEV9write16(u32 addr, u16 value) {}
void DEV9write32(u32 addr, u32 value) {}
void DEV9CheckChanges(const Pcsx2Config& old_config) {}

namespace USB
{
	s32 DeviceTypeNameToIndex(const std::string_view device) { return 0; }
	const char* DeviceTypeIndexToName(s32 device) { return ""; }
	std::vector<std::pair<const char*, const char*>> GetDeviceTypes() { return {}; }
	const char* GetDeviceName(const std::string_view device) { return ""; }
	const char* GetDeviceIconName(u32 port) { return ""; }
	const char* GetDeviceSubtypeName(const std::string_view device, u32 subtype) { return ""; }
	std::span<const char*> GetDeviceSubtypes(const std::string_view device) { return {}; }
	std::span<const InputBindingInfo> GetDeviceBindings(const std::string_view device, u32 subtype) { return {}; }
	std::span<const SettingInfo> GetDeviceSettings(const std::string_view device, u32 subtype) { return {}; }
	std::span<const InputBindingInfo> GetDeviceBindings(u32 port) { return {}; }
	float GetDeviceBindValue(u32 port, u32 bind_index) { return 0.0f; }
	void SetDeviceBindValue(u32 port, u32 bind_index, float value) {}
	void InputDeviceConnected(const std::string_view identifier) {}
	void InputDeviceDisconnected(const std::string_view identifier) {}
	std::string GetConfigSection(int port) { return {}; }
	std::string GetConfigDevice(const SettingsInterface& si, u32 port) { return {}; }
	void SetConfigDevice(SettingsInterface& si, u32 port, const char* devname) {}
	u32 GetConfigSubType(const SettingsInterface& si, u32 port, const std::string_view devname) { return 0; }
	void SetConfigSubType(SettingsInterface& si, u32 port, const std::string_view devname, u32 subtype) {}
	std::string GetConfigSubKey(const std::string_view device, const std::string_view bind_name) { return {}; }
	bool MapDevice(SettingsInterface& si, u32 port, const std::vector<std::pair<GenericInputBinding, std::string>>& mapping) { return false; }
	void ClearPortBindings(SettingsInterface& si, u32 port) {}
	void CopyConfiguration(SettingsInterface* dest_si, const SettingsInterface& src_si, bool copy_devices, bool copy_bindings) {}
	void SetDefaultConfiguration(SettingsInterface* si) {}
	void CheckForConfigChanges(const Pcsx2Config& old_config) {}
	bool ConfigKeyExists(SettingsInterface& si, u32 port, const char* devname, const char* key) { return false; }
	bool GetConfigBool(SettingsInterface& si, u32 port, const char* devname, const char* key, bool default_value) { return default_value; }
	s32 GetConfigInt(SettingsInterface& si, u32 port, const char* devname, const char* key, s32 default_value) { return default_value; }
	float GetConfigFloat(SettingsInterface& si, u32 port, const char* devname, const char* key, float default_value) { return default_value; }
	std::string GetConfigString(SettingsInterface& si, u32 port, const char* devname, const char* key, const char* default_value) { return default_value; }
	bool DoState(StateWrapper& sw) { return true; }
} // namespace USB

void USBinit() {}
void USBasync(u32 cycles) {}
void USBshutdown() {}
void USBclose() {}
bool USBopen() { return true; }
void USBreset() {}
u8 USBread8(u32 addr) { return 0; }
u16 USBread16(u32 addr) { return 0; }
u32 USBread32(u32 addr) { return 0; }
void USBwrite8(u32 addr, u8 value) {}
void USBwrite16(u32 addr, u16 value) {}
void USBwrite32(u32 addr, u32 value) {}
void USBsetRAM(void* mem) {}

s32 FWopen() { return 0; }
void FWclose() {}
void PHYWrite() {}
void PHYRead() {}
u32 FWread32(u32 addr) { return fwRu32(addr); }
void FWwrite32(u32 addr, u32 value) { fwRu32(addr) = value; }
