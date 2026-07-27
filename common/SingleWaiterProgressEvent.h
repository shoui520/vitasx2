// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Assertions.h"
#include "common/Threading.h"

#include <atomic>

namespace Threading
{
	// Sleeps one producer until a separately published progress value changes.
	//
	// The consumer calls NotifyOfProgress() after an ordinary progress
	// publication and PublishQuiescence() before it can stop publishing. The
	// latter performs an unconditional release RMW even when no waiter is
	// armed. A producer which arms afterwards acquires that RMW before
	// rechecking progress, closing the otherwise possible publish-before-arm
	// lost-wakeup race without an atomic RMW on every consumer item.
	class SingleWaiterProgressEvent final
	{
	public:
		template <typename Progress, typename ReadProgress>
		void WaitForChange(Progress observed, ReadProgress&& read_progress)
		{
			bool expected = false;
			pxAssertRel(m_waiting.compare_exchange_strong(expected, true,
							std::memory_order_acq_rel,
							std::memory_order_relaxed),
				"SingleWaiterProgressEvent has more than one waiter");

			if (read_progress() != observed)
			{
				// If cancellation loses, the consumer already claimed this
				// waiter and its semaphore post must be consumed.
				if (m_waiting.exchange(false, std::memory_order_acq_rel))
					return;
			}
			m_semaphore.Wait();
		}

		void NotifyOfProgress()
		{
			if (m_waiting.load(std::memory_order_relaxed) &&
				m_waiting.exchange(false, std::memory_order_acq_rel))
			{
				m_semaphore.Post();
			}
		}

		void PublishQuiescence()
		{
			if (m_waiting.exchange(false, std::memory_order_acq_rel))
				m_semaphore.Post();
		}

		bool IsWaitingForValidation() const
		{
			return m_waiting.load(std::memory_order_acquire);
		}

	private:
		std::atomic_bool m_waiting{false};
		UserspaceSemaphore m_semaphore;
	};
} // namespace Threading
