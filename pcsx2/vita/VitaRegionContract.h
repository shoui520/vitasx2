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
		GuestGprLow32,
		IopPublishedEventCountdown,
		IopEeBudget,
	};

	struct ResidentBinding
	{
		ResidentValue value = ResidentValue::None;
		u8 guest = 0;

		[[nodiscard]] constexpr bool operator==(const ResidentBinding& rhs) const
		{
			return value == rhs.value &&
				(value != ResidentValue::GuestGprLow32 || guest == rhs.guest);
		}
	};

	struct EntryContract
	{
		static constexpr unsigned HostRegisterCount = 13;

		GuestDomain domain = GuestDomain::None;
		std::array<ResidentBinding, HostRegisterCount> host_values{};

		constexpr void Bind(unsigned host, ResidentValue value, u8 guest = 0)
		{
			if (host < host_values.size())
				host_values[host] = {value, guest};
		}

		[[nodiscard]] constexpr bool Provides(const EntryContract& required) const
		{
			if (domain == GuestDomain::None || domain != required.domain)
				return false;

			for (unsigned host = 0; host < host_values.size(); host++)
			{
				if (required.host_values[host].value != ResidentValue::None &&
					!(host_values[host] == required.host_values[host]))
				{
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] constexpr bool Contains(ResidentValue value) const
		{
			for (const ResidentBinding& binding : host_values)
			{
				if (binding.value == value)
					return true;
			}
			return false;
		}
	};
} // namespace VitaRegion
