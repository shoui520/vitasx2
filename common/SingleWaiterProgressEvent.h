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
	// Arming and waking are serialized so a semaphore token can never be
	// confused with a later arm generation. The quiescence lock closes the
	// publish-before-arm lost-wakeup race without locking every consumer item.
	class SingleWaiterProgressEvent final
	{
	public:
		template <typename Progress, typename ReadProgress>
		void WaitForChange(Progress observed, ReadProgress&& read_progress)
		{
			{
				std::lock_guard lock(m_mutex);
				pxAssertRel(!m_waiting,
					"SingleWaiterProgressEvent has more than one waiter");
				if (read_progress() != observed)
					return;
				m_waiting = true;
				m_waiting_hint.store(true, std::memory_order_release);
			}
			m_semaphore.Wait();
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

	private:
		void WakeWaiter()
		{
			bool post = false;
			{
				std::lock_guard lock(m_mutex);
				if (m_waiting)
				{
					m_waiting = false;
					m_waiting_hint.store(false, std::memory_order_release);
					post = true;
				}
			}
			if (post)
				m_semaphore.Post();
		}

		std::atomic_bool m_waiting_hint{false};
		mutable KernelMutex m_mutex;
		bool m_waiting = false;
		KernelSemaphore m_semaphore;
	};
} // namespace Threading
