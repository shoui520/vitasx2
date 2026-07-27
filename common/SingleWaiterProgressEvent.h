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
	// publication and PublishQuiescence() before it can stop publishing.
	// Ordinary progress pays only a relaxed hint load when nobody is waiting.
	// Wait calls are serialized, and an arm remains owned until its waiter
	// acknowledges the claimed wake. An unrelated semaphore token can
	// therefore neither complete the arm nor be confused with a later
	// generation. The quiescence lock closes the publish-before-arm
	// lost-wakeup race without locking every consumer item.
	class SingleWaiterProgressEvent final
	{
	public:
		template <typename Progress, typename ReadProgress>
		void WaitForChange(Progress observed, ReadProgress&& read_progress)
		{
			// Most owners have one producer, but lifecycle and observation
			// boundaries can overlap it. Keep exactly one physical semaphore
			// waiter without making those legal callers race for one token.
			std::lock_guard waiter_lock(m_waiter_mutex);
			{
				std::lock_guard lock(m_mutex);
				pxAssertRel(!m_waiting && !m_wake_claimed,
					"progress-event wake acknowledgement is incomplete");
				if (read_progress() != observed)
					return;
				m_waiting = true;
				m_wake_claimed = false;
				m_waiting_hint.store(true, std::memory_order_release);
			}

			for (;;)
			{
				m_semaphore.Wait();
				std::lock_guard lock(m_mutex);
				if (!m_wake_claimed)
				{
					// A semaphore resource is not a generation identifier.
					// Ignore an unclaimed resource instead of returning with
					// this arm still live and poisoning the next wait.
					continue;
				}

				m_waiting = false;
				m_wake_claimed = false;
				// If an unclaimed resource woke this thread immediately before
				// the consumer posted the claimed one, remove that redundant
				// resource before the next generation can arm.
				while (m_semaphore.TryWait())
				{
				}
				return;
			}
		}

		void NotifyOfProgress()
		{
			if (m_waiting_hint.load(std::memory_order_relaxed))
				WakeWaiter();
		}

		void PublishQuiescence()
		{
			// Always take the lock. If progress was published before the
			// producer armed, this unlock/lock pair makes that publication
			// visible to its protected recheck. If it armed first, wake it.
			WakeWaiter();
		}

		bool IsWaitingForValidation() const
		{
			return m_waiting_hint.load(std::memory_order_acquire);
		}

#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION
		void PostUnclaimedTokenForValidation()
		{
			m_semaphore.Post();
		}
#endif

	private:
		void WakeWaiter()
		{
			std::lock_guard lock(m_mutex);
			if (m_waiting && !m_wake_claimed)
			{
				m_wake_claimed = true;
				m_waiting_hint.store(false, std::memory_order_release);
				// Keep the state lock through publication. A waiter which
				// consumed an older resource cannot acknowledge this wake
				// until its corresponding resource has also been posted.
				m_semaphore.Post();
			}
		}

		std::atomic_bool m_waiting_hint{false};
		mutable KernelMutex m_waiter_mutex;
		mutable KernelMutex m_mutex;
		bool m_waiting = false;
		bool m_wake_claimed = false;
		KernelSemaphore m_semaphore;
	};
} // namespace Threading
