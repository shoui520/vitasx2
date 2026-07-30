// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <array>

namespace VitaRegion
{
	enum class GuestDomain : u8
	{
		None,
		Iop,
		Ee,
	};

	// Frontends describe values which a generated internal entry expects to
	// remain in host registers. Matching happens while code is linked; no
	// runtime abstraction is emitted on the Cortex-A9 path.
	enum class ResidentValue : u8
	{
		None,
		CoreStateBase,
		CycleStateBase,
		MainMemoryBase,
		MainMemoryMask,
	};

	struct EntryContract
	{
		static constexpr unsigned HostRegisterCount = 13;

		GuestDomain domain = GuestDomain::None;
		std::array<ResidentValue, HostRegisterCount> host_values{};

		constexpr void Bind(unsigned host, ResidentValue value)
		{
			if (host < host_values.size())
				host_values[host] = value;
		}

		[[nodiscard]] constexpr bool Provides(const EntryContract& required) const
		{
			if (domain == GuestDomain::None || domain != required.domain)
				return false;

			for (unsigned host = 0; host < host_values.size(); host++)
			{
				if (required.host_values[host] != ResidentValue::None &&
					host_values[host] != required.host_values[host])
				{
					return false;
				}
			}
			return true;
		}
	};
} // namespace VitaRegion
