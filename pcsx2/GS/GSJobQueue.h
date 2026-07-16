// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS.h"
#include "common/boost_spsc_queue.hpp"
#include "common/Assertions.h"
#include "common/Threading.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>

template <class T, int CAPACITY>
class GSJobQueue final
{
private:
#if defined(__vita__)
	Threading::Thread m_thread;
#else
	std::thread m_thread;
#endif
	std::function<void()> m_startup;
	std::function<void(T&)> m_func;
	std::function<void()> m_shutdown;
	bool m_exit;
	ringbuffer_base<T, CAPACITY> m_queue;

	Threading::WorkSema m_sema;

	void ThreadProc()
	{
		if (m_startup)
			m_startup();

		while (true)
		{
			m_sema.WaitForWorkWithSpin();
			if (m_exit)
				break;
			while (m_queue.consume_one(*this))
				;
		}

		if (m_shutdown)
			m_shutdown();
	}

public:
	GSJobQueue(std::function<void()> startup, std::function<void(T&)> func, std::function<void()> shutdown)
		: m_startup(std::move(startup))
		, m_func(std::move(func))
		, m_shutdown(std::move(shutdown))
		, m_exit(false)
	{
#if defined(__vita__)
		// Sony's thread-manager API requires an explicit stack size and CPU
		// affinity contract. Vita's Threading::Thread supplies the PSP2/pte
		// lifetime handshake; the rasterizer owner narrows it to USER_2 from its
		// startup callback.
		m_thread.SetStackSize(256 * 1024);
		if (!m_thread.Start([this]() { ThreadProc(); }))
			throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
				"failed to start Vita GS job worker");
#else
		m_thread = std::thread(&GSJobQueue::ThreadProc, this);
#endif
	}

	~GSJobQueue()
	{
		m_exit = true;
		m_sema.NotifyOfWork();
	#if defined(__vita__)
		m_thread.Join();
	#else
		m_thread.join();
	#endif
	}

	bool IsEmpty()
	{
		return m_queue.empty();
	}

	void Push(const T& item)
	{
		while (!m_queue.push(item))
	#if defined(__vita__)
			Threading::Timeslice();
	#else
			std::this_thread::yield();
	#endif
		m_sema.NotifyOfWork();
	}

	void Wait()
	{
		m_sema.WaitForEmptyWithSpin();
		pxAssert(IsEmpty());
	}

	void operator()(T& item)
	{
		m_func(item);
	}
};
