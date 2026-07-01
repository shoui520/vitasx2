// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Vita thread layer. Threads are pthread-embedded (pte) threads, which are
// SceKernel threads underneath; the SceUID captured at start time drives
// affinity/priority through the native kernel API.

#include "common/Threading.h"
#include "common/Assertions.h"

#include <memory>

#include <pthread.h>
#include <sched.h>

#include <psp2/kernel/threadmgr.h>

__forceinline void Threading::Timeslice()
{
	sched_yield();
}

__forceinline void Threading::SpinWait()
{
	// ARMv7 yield hint; a NOP on Cortex-A9, but keeps intent explicit.
	__asm__ __volatile__("yield");
}

__forceinline void Threading::EnableHiresScheduler()
{
	// The Vita scheduler resolution is not adjustable.
}

__forceinline void Threading::DisableHiresScheduler()
{
}

// Unit of time of GetThreadCpuTime/GetCpuTime
u64 Threading::GetThreadTicksPerSecond()
{
	return 1000000;
}

u64 Threading::GetThreadCpuTime()
{
	// No per-thread CPU clock is exposed to user code; this feeds the
	// performance overlay only. Report zero rather than lying.
	return 0;
}

Threading::ThreadHandle::ThreadHandle() = default;

Threading::ThreadHandle::ThreadHandle(const ThreadHandle& handle)
	: m_native_handle(handle.m_native_handle)
	, m_native_id(handle.m_native_id)
{
}

Threading::ThreadHandle::ThreadHandle(ThreadHandle&& handle)
	: m_native_handle(handle.m_native_handle)
	, m_native_id(handle.m_native_id)
{
	handle.m_native_handle = nullptr;
	handle.m_native_id = 0;
}

Threading::ThreadHandle::~ThreadHandle() = default;

Threading::ThreadHandle Threading::ThreadHandle::GetForCallingThread()
{
	ThreadHandle ret;
	ret.m_native_handle = (void*)(uptr)pthread_self();
	ret.m_native_id = static_cast<unsigned int>(sceKernelGetThreadId());
	return ret;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(ThreadHandle&& handle)
{
	m_native_handle = handle.m_native_handle;
	handle.m_native_handle = nullptr;
	m_native_id = handle.m_native_id;
	handle.m_native_id = 0;
	return *this;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(const ThreadHandle& handle)
{
	m_native_handle = handle.m_native_handle;
	m_native_id = handle.m_native_id;
	return *this;
}

u64 Threading::ThreadHandle::GetCPUTime() const
{
	return 0;
}

bool Threading::ThreadHandle::SetAffinity(u64 processor_mask) const
{
	if (m_native_id == 0)
		return false;

	// User threads may run on cores 0-2 (bit 16..18 of the kernel mask); a
	// kernel plugin is required for core 3. Mask bits beyond what the kernel
	// grants are rejected by the call itself.
	int mask = 0;
	if (processor_mask != 0)
	{
		for (u32 i = 0; i < 4; i++)
		{
			if (processor_mask & (static_cast<u64>(1) << i))
				mask |= (0x10000 << i);
		}
	}
	else
	{
		mask = 0; // 0 = default/all allowed cores
	}

	return sceKernelChangeThreadCpuAffinityMask(static_cast<SceUID>(m_native_id), mask) >= 0;
}

Threading::Thread::Thread() = default;

Threading::Thread::Thread(Thread&& thread)
	: ThreadHandle(thread)
	, m_stack_size(thread.m_stack_size)
{
	thread.m_stack_size = 0;
}

Threading::Thread::Thread(EntryPoint func)
	: ThreadHandle()
{
	if (!Start(std::move(func)))
		pxFailRel("Failed to start implicitly started thread.");
}

Threading::Thread::~Thread()
{
	pxAssertRel(!m_native_handle, "Thread should be detached or joined at destruction");
}

void Threading::Thread::SetStackSize(u32 size)
{
	pxAssertRel(!m_native_handle, "Can't change the stack size on a started thread");
	m_stack_size = size;
}

// Like the Linux implementation, the SceUID of the new thread is reported back
// through a semaphore handshake, because pte does not expose it.
struct ThreadProcParameters
{
	Threading::Thread::EntryPoint func;
	Threading::KernelSemaphore* start_semaphore;
	unsigned int* thread_id_ptr;
};

void* Threading::Thread::ThreadProc(void* param)
{
	std::unique_ptr<ThreadProcParameters> entry(static_cast<ThreadProcParameters*>(param));
	*entry->thread_id_ptr = static_cast<unsigned int>(sceKernelGetThreadId());
	entry->start_semaphore->Post();
	entry->func();
	return nullptr;
}

bool Threading::Thread::Start(EntryPoint func)
{
	pxAssertRel(!m_native_handle, "Can't start an already-started thread");

	KernelSemaphore start_semaphore;
	std::unique_ptr<ThreadProcParameters> params(std::make_unique<ThreadProcParameters>());
	params->func = std::move(func);
	params->start_semaphore = &start_semaphore;
	params->thread_id_ptr = &m_native_id;

	pthread_attr_t attrs;
	bool has_attributes = false;

	if (m_stack_size != 0)
	{
		has_attributes = true;
		pthread_attr_init(&attrs);
		pthread_attr_setstacksize(&attrs, m_stack_size);
	}

	pthread_t handle;
	const int res = pthread_create(&handle, has_attributes ? &attrs : nullptr, ThreadProc, params.get());
	if (res != 0)
		return false;

	// wait until it sets our native id
	start_semaphore.Wait();

	// thread started, it'll release the memory
	m_native_handle = (void*)(uptr)handle;
	params.release();
	return true;
}

void Threading::Thread::Detach()
{
	pxAssertRel(m_native_handle, "Can't detach without a thread");
	pthread_detach((pthread_t)(uptr)m_native_handle);
	m_native_handle = nullptr;
	m_native_id = 0;
}

void Threading::Thread::Join()
{
	pxAssertRel(m_native_handle, "Can't join without a thread");
	void* retval;
	const int res = pthread_join((pthread_t)(uptr)m_native_handle, &retval);
	if (res != 0)
		pxFailRel("pthread_join() for thread join failed");

	m_native_handle = nullptr;
	m_native_id = 0;
}

Threading::ThreadHandle& Threading::Thread::operator=(Thread&& thread)
{
	ThreadHandle::operator=(thread);
	m_stack_size = thread.m_stack_size;
	thread.m_stack_size = 0;
	return *this;
}

void Threading::SetNameOfCurrentThread(const char* name)
{
	// Sce thread names are fixed at creation; pte owns creation. Not fatal.
	(void)name;
}
