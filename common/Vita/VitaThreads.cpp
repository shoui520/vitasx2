// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Vita thread layer. Threads are pthread-embedded (pte) threads, which are
// SceKernel threads underneath; the SceUID captured at start time drives
// affinity/priority through the native kernel API.

#include "common/Threading.h"
#include "common/Assertions.h"

#include <memory>
#include <utility>

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

static u64 GetVitaThreadCpuTime(SceUID thread_id)
{
	if (thread_id < 0)
		return 0;

	SceKernelThreadInfo info = {};
	info.size = sizeof(info);
	if (sceKernelGetThreadInfo(thread_id, &info) < 0)
		return 0;

	// Sony documents runClocks as cumulative thread execution time in
	// microseconds, matching GetThreadTicksPerSecond().
	return static_cast<u64>(info.runClocks);
}

u64 Threading::GetThreadCpuTime()
{
	return GetVitaThreadCpuTime(static_cast<SceUID>(sceKernelGetThreadId()));
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
	return (m_native_id != 0) ?
		GetVitaThreadCpuTime(static_cast<SceUID>(m_native_id)) : 0;
}

bool Threading::ThreadHandle::SetAffinity(u64 processor_mask) const
{
	if (m_native_id == 0)
		return false;

	// Sony's thread-manager contract exposes only USER_0..USER_2 to game
	// applications.  Keep that contract even when CapUnlocker expands the
	// process default: CPU3 owns shell, plugin, and background work.
	constexpr u64 user_processor_mask = 0x7;
	if ((processor_mask & ~user_processor_mask) != 0)
		return false;
	const bool user_all = processor_mask == 0;
	const bool one_cpu = !user_all &&
		(processor_mask & (processor_mask - 1u)) == 0u;
	// Sony's scheduler explicitly recommends only individual-priority + one CPU
	// or common-priority + USER_ALL. Do not silently admit a low-determinism
	// multi-CPU subset which belongs to neither contract.
	if (!user_all && !one_cpu)
		return false;

	int mask = SCE_KERNEL_CPU_MASK_USER_ALL;
	if (!user_all)
	{
		mask = 0;
		if (processor_mask & (1u << 0))
			mask |= SCE_KERNEL_CPU_MASK_USER_0;
		if (processor_mask & (1u << 1))
			mask |= SCE_KERNEL_CPU_MASK_USER_1;
		if (processor_mask & (1u << 2))
			mask |= SCE_KERNEL_CPU_MASK_USER_2;
	}

	const SceUID thread_id = static_cast<SceUID>(m_native_id);
	SceKernelThreadInfo info{};
	info.size = sizeof(info);
	if (sceKernelGetThreadInfo(thread_id, &info) < 0)
		return false;

	// Priorities 64..127 use a CPU's individual ready queue; 128..191 use
	// USER_ALL's common queue. Preserve the caller's relative priority while
	// moving it to the queue class required by the requested affinity.
	constexpr int individual_priority_begin = 64;
	constexpr int individual_priority_end = 127;
	constexpr int common_priority_begin = 128;
	constexpr int common_priority_end = 191;
	const int old_priority = info.currentPriority;
	int new_priority = old_priority;
	if (one_cpu && old_priority >= common_priority_begin &&
		old_priority <= common_priority_end)
	{
		new_priority = old_priority - 64;
	}
	else if (user_all && old_priority >= individual_priority_begin &&
		old_priority <= individual_priority_end)
	{
		new_priority = old_priority + 64;
	}

	if (one_cpu)
	{
		// Promote into the individual queue before restricting affinity. Sony's
		// scheduler always drains a CPU's individual queue before its common queue;
		// restricting a runnable common-priority thread first can therefore starve
		// the thread before it executes the second half of a self-pin operation.
		// The temporary USER_ALL/individual pairing lasts for one syscall only.
		if (new_priority != old_priority &&
			sceKernelChangeThreadPriority(thread_id, new_priority) < 0)
			return false;
		if (sceKernelChangeThreadCpuAffinityMask(thread_id, mask) < 0)
		{
			if (new_priority != old_priority)
				(void)sceKernelChangeThreadPriority(thread_id, old_priority);
			return false;
		}
		return true;
	}

	// Widen affinity before demoting to the common queue for the symmetric
	// reason: a common-priority thread restricted to one busy CPU may never run
	// the widening syscall. Restore the prior affinity if demotion fails.
	const int prior_affinity =
		sceKernelChangeThreadCpuAffinityMask(thread_id, mask);
	if (prior_affinity < 0)
		return false;
	if (new_priority != old_priority &&
		sceKernelChangeThreadPriority(thread_id, new_priority) < 0)
	{
		(void)sceKernelChangeThreadCpuAffinityMask(
			thread_id, prior_affinity);
		return false;
	}
	return true;
}

Threading::Thread::Thread() = default;

Threading::Thread::Thread(Thread&& thread)
	: ThreadHandle(std::move(thread))
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
	bool* start_succeeded;
};

void* Threading::Thread::ThreadProc(void* param)
{
	std::unique_ptr<ThreadProcParameters> entry(static_cast<ThreadProcParameters*>(param));
	const SceUID thread_id = sceKernelGetThreadId();
	// The official default normally means USER_ALL, but an installed capability
	// plugin can widen it. Establish Sony's common-priority + USER_ALL pairing
	// before any emulator entry point executes; children can inherit an
	// individual priority from a pinned parent, so changing only the mask would
	// create the low-determinism queue/affinity combination the SDK warns about.
	// Owners may narrow both queue class and affinity through SetAffinity() after
	// Start() returns.
	const Threading::ThreadHandle self =
		Threading::ThreadHandle::GetForCallingThread();
	const bool normalized = self.SetAffinity(0);
	*entry->thread_id_ptr = static_cast<unsigned int>(thread_id);
	*entry->start_succeeded = normalized;
	entry->start_semaphore->Post();
	if (!normalized)
		return nullptr;
	entry->func();
	return nullptr;
}

bool Threading::Thread::Start(EntryPoint func)
{
	pxAssertRel(!m_native_handle, "Can't start an already-started thread");

	KernelSemaphore start_semaphore;
	bool start_succeeded = false;
	std::unique_ptr<ThreadProcParameters> params(std::make_unique<ThreadProcParameters>());
	params->func = std::move(func);
	params->start_semaphore = &start_semaphore;
	params->thread_id_ptr = &m_native_id;
	params->start_succeeded = &start_succeeded;

	pthread_attr_t attrs;
	pthread_attr_t* attrs_ptr = nullptr;

	if (m_stack_size != 0)
	{
		if (pthread_attr_init(&attrs) != 0)
			return false;

		if (pthread_attr_setstacksize(&attrs, m_stack_size) != 0)
		{
			const int destroy_res = pthread_attr_destroy(&attrs);
			pxAssertRel(destroy_res == 0, "pthread_attr_destroy() failed after stack-size rejection");
			return false;
		}

		attrs_ptr = &attrs;
	}

	pthread_t handle;
	const int res = pthread_create(&handle, attrs_ptr, ThreadProc, params.get());
	if (attrs_ptr)
	{
		const int destroy_res = pthread_attr_destroy(attrs_ptr);
		pxAssertRel(destroy_res == 0, "pthread_attr_destroy() failed after thread creation");
	}
	if (res != 0)
		return false;

	// pthread_create succeeded, so the child exclusively owns and destroys the
	// parameter block. Release the parent's failure-path owner before waiting;
	// a startup rejection or immediately returning entry point may otherwise
	// delete it before this thread reaches the old post-wait release.
	params.release();

	// wait until it sets our native id
	start_semaphore.Wait();
	if (!start_succeeded)
	{
		void* retval = nullptr;
		(void)pthread_join(handle, &retval);
		m_native_id = 0;
		return false;
	}

	// Thread startup and Sony queue/affinity normalization both succeeded.
	m_native_handle = (void*)(uptr)handle;
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
	if (this == &thread)
		return *this;

	pxAssertRel(!m_native_handle, "Can't move-assign over a joinable thread");
	ThreadHandle::operator=(std::move(thread));
	m_stack_size = thread.m_stack_size;
	thread.m_stack_size = 0;
	return *this;
}

void Threading::SetNameOfCurrentThread(const char* name)
{
	// Sce thread names are fixed at creation; pte owns creation. Not fatal.
	(void)name;
}
