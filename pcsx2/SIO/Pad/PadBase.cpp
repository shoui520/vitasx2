// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "SIO/Pad/PadBase.h"

#include <limits>

PadBase::PadBase(u8 unifiedSlot, size_t ejectTicks)
{
	this->unifiedSlot = unifiedSlot;
	this->ejectTicks = ejectTicks;
}

PadBase::~PadBase() = default;

void PadBase::SoftReset()
{
	commandBytesReceived = 1;
}

void PadBase::FullReset()
{
	this->isInConfig = false;
	this->currentMode = Pad::Mode::DIGITAL;
}

bool PadBase::Freeze(StateWrapper& sw)
{
	if (!sw.DoMarker("PadBase"))
		return false;

	// Protected PadBase members
	sw.Do(&unifiedSlot);
	sw.Do(&isInConfig);
	sw.Do(&currentMode);
	sw.Do(&currentCommand);
	if (sw.IsPortableReplay())
	{
		if (sw.IsWriting() && commandBytesReceived > std::numeric_limits<u32>::max())
			return false;
		u32 portable_command_bytes = static_cast<u32>(commandBytesReceived);
		sw.Do(&portable_command_bytes);
		if (sw.IsReading())
			commandBytesReceived = portable_command_bytes;
	}
	else
	{
		sw.Do(&commandBytesReceived);
	}
	return !sw.HasError();
}
