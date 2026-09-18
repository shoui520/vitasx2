// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuHealthJournal.h"

#include "common/Threading.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

#if defined(__vita__)
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/net/net.h>
#include <psp2/sysmodule.h>
#endif

namespace VitaGpuVu::HealthJournal
{
	namespace
	{
		constexpr size_t HeaderSize = 64u;
		constexpr size_t CrcOffset = 60u;
		constexpr u16 SectionVersion = 1u;
		constexpr u32 CoherentSectionFlag = 1u << 0;
		constexpr u32 SectionReadAttempts = 4u;
		static_assert(std::atomic<u32>::is_always_lock_free && std::atomic<u64>::is_always_lock_free,
			"Health observation must not fall back to library locks on Cortex-A9");

		enum class SectionKind : u16
		{
			GxmCall = 1,
			Mtgs = 2,
			Retirement = 3,
			Notification = 4,
			Cpu1 = 5,
			Lifecycle = 6,
			Cpu0 = 7,
			Handoff = 8,
			ThreadActivity = 9,
			Writer = 10,
			Probe = 11,
			NoOutputExecutions = 12,
		};

		constexpr size_t GxmValueCount = 35u;
		constexpr size_t MtgsValueCount = 19u;
		constexpr size_t RetirementSlotValueCount = 14u;
		constexpr size_t RetirementValueCount = 5u + 4u * RetirementSlotValueCount;
		constexpr size_t NotificationValueCount = 18u;
		constexpr size_t Cpu1ValueCount = 11u;
		constexpr size_t LifecycleValueCount = 9u;
		constexpr size_t Cpu0ValueCount = 14u;
		constexpr size_t HandoffValueCount = 4u;
		constexpr size_t ThreadValueCount = 1u + 5u * 8u;
		constexpr size_t WriterValueCount = 10u;
		constexpr size_t ProbeValueCount = 12u;
		constexpr size_t NoOutputValueCount = static_cast<size_t>(NoOutputExecutionKind::Count);
		constexpr size_t EncodedPayloadSize =
			12u * 16u + sizeof(u64) * (GxmValueCount + MtgsValueCount +
				RetirementValueCount + NotificationValueCount + Cpu1ValueCount +
				LifecycleValueCount + Cpu0ValueCount + HandoffValueCount + ThreadValueCount +
				WriterValueCount + ProbeValueCount + NoOutputValueCount);
		static_assert(HeaderSize + EncodedPayloadSize <= RecordSize);

		template <size_t ValueCount>
		struct AtomicSection
		{
			std::atomic<u32> sequence{0};
			std::array<std::atomic<u64>, ValueCount> values{};

			void Publish(const std::array<u64, ValueCount>& source)
			{
				const u32 previous = sequence.fetch_add(
					1u, std::memory_order_acq_rel);
				for (size_t i = 0; i < ValueCount; i++)
					values[i].store(source[i], std::memory_order_relaxed);
				sequence.store(previous + 2u, std::memory_order_release);
			}

			bool Read(std::array<u64, ValueCount>* destination,
				u32* coherent_sequence) const
			{
				for (u32 attempt = 0; attempt < SectionReadAttempts; attempt++)
				{
					const u32 before = sequence.load(std::memory_order_acquire);
					if ((before & 1u) != 0u)
						continue;
					std::array<u64, ValueCount> candidate{};
					for (size_t i = 0; i < ValueCount; i++)
					{
						candidate[i] = values[i].load(
							std::memory_order_relaxed);
					}
					std::atomic_thread_fence(std::memory_order_acquire);
					const u32 after = sequence.load(std::memory_order_acquire);
					if (before == after && (after & 1u) == 0u)
					{
						*destination = candidate;
						*coherent_sequence = after;
						return true;
					}
				}
				return false;
			}

			void Reset()
			{
				sequence.store(0u, std::memory_order_relaxed);
				for (std::atomic<u64>& value : values)
					value.store(0u, std::memory_order_relaxed);
			}
		};

		AtomicSection<GxmValueCount> s_gxm;
		AtomicSection<MtgsValueCount> s_mtgs;
		AtomicSection<RetirementValueCount> s_retirement;
		AtomicSection<NotificationValueCount> s_notification;
		AtomicSection<Cpu1ValueCount> s_cpu1;
		AtomicSection<LifecycleValueCount> s_lifecycle;
		AtomicSection<Cpu0ValueCount> s_cpu0;
		AtomicSection<WriterValueCount> s_writer_activity;
		AtomicSection<ProbeValueCount> s_probe_activity;
		AtomicSection<ThreadValueCount> s_thread_activity;
		std::array<std::atomic<u64>, HandoffValueCount> s_handoff{};
		std::array<std::atomic<u64>, NoOutputValueCount> s_no_output_executions{};
		std::array<std::atomic<u32>, 5> s_thread_ids{};
		void RegisterOwnerThread(size_t owner)
		{
#if defined(__vita__)
			if (s_thread_ids[owner].load(std::memory_order_relaxed) == 0)
				s_thread_ids[owner].store(static_cast<u32>(sceKernelGetThreadId()),
					std::memory_order_relaxed);
#endif
		}

		std::atomic<u64> s_run_id{0};
		std::atomic<u64> s_last_sequence{0};
		std::atomic<u32> s_writer_running{0};
		std::atomic<u32> s_open_failures{0};
		std::atomic<u32> s_write_failures{0};
		std::atomic<u32> s_sync_failures{0};
		std::atomic<u64> s_flush_requested{0};
		std::atomic<u64> s_flush_completed{0};
		std::array<u8, 16> s_live_token{};
		u32 s_live_port = 0;

#if defined(__vita__)
		Threading::Thread s_writer_thread;
		Threading::KernelSemaphore s_writer_startup_sema;
		std::atomic<u32> s_writer_startup_result{0};
		std::atomic<bool> s_writer_shutdown{false};
		alignas(64) std::array<u8, RecordSize> s_record_buffer{};
		Threading::Thread s_live_thread;
		std::atomic<bool> s_live_shutdown{false};
		WriterActivity s_writer_state;
		ProbeActivity s_probe_state;
#endif

		constexpr std::array<u32, 256> MakeCrc32Table()
		{
			std::array<u32, 256> table{};
			for (u32 index = 0; index < table.size(); index++)
			{
				u32 value = index;
				for (u32 bit = 0; bit < 8u; bit++)
				{
					value = (value >> 1u) ^
						((value & 1u) != 0u ? 0xedb88320u : 0u);
				}
				table[index] = value;
			}
			return table;
		}

		constexpr std::array<u32, 256> Crc32Table = MakeCrc32Table();

		u32 CalculateCrc32(const u8* bytes, size_t size)
		{
			u32 crc = 0xffffffffu;
			for (size_t index = 0; index < size; index++)
			{
				crc = Crc32Table[(crc ^ bytes[index]) & 0xffu] ^
					(crc >> 8u);
			}
			return crc ^ 0xffffffffu;
		}

		void WriteLe16(u8* destination, u16 value)
		{
			destination[0] = static_cast<u8>(value);
			destination[1] = static_cast<u8>(value >> 8u);
		}

		void WriteLe32(u8* destination, u32 value)
		{
			for (u32 index = 0; index < 4u; index++)
				destination[index] = static_cast<u8>(value >> (index * 8u));
		}

		void WriteLe64(u8* destination, u64 value)
		{
			for (u32 index = 0; index < 8u; index++)
				destination[index] = static_cast<u8>(value >> (index * 8u));
		}

		u16 ReadLe16(const u8* source)
		{
			return static_cast<u16>(source[0]) |
				(static_cast<u16>(source[1]) << 8u);
		}

		u32 ReadLe32(const u8* source)
		{
			u32 value = 0;
			for (u32 index = 0; index < 4u; index++)
				value |= static_cast<u32>(source[index]) << (index * 8u);
			return value;
		}

		u64 ReadLe64(const u8* source)
		{
			u64 value = 0;
			for (u32 index = 0; index < 8u; index++)
				value |= static_cast<u64>(source[index]) << (index * 8u);
			return value;
		}

		struct RecordBuilder
		{
			u8* bytes;
			size_t offset = HeaderSize;
			bool okay = true;

			void WriteSection(SectionKind kind, u32 owner_sequence,
				bool coherent, const u64* values, size_t value_count)
			{
				const size_t section_size = 16u + value_count * sizeof(u64);
				if (!okay || section_size > RecordSize - offset)
				{
					okay = false;
					return;
				}
				WriteLe16(bytes + offset, static_cast<u16>(kind));
				WriteLe16(bytes + offset + 2u, SectionVersion);
				WriteLe32(bytes + offset + 4u,
					static_cast<u32>(section_size));
				WriteLe32(bytes + offset + 8u, owner_sequence);
				WriteLe32(bytes + offset + 12u,
					coherent ? CoherentSectionFlag : 0u);
				offset += 16u;
				for (size_t index = 0; index < value_count; index++)
				{
					WriteLe64(bytes + offset, values[index]);
					offset += sizeof(u64);
				}
			}
		};

		template <size_t ValueCount>
		void AppendSection(RecordBuilder* builder, SectionKind kind,
			const AtomicSection<ValueCount>& source, u32 section_bit,
			u32* valid_mask, u32* incoherent_mask)
		{
			std::array<u64, ValueCount> values{};
			u32 sequence = 0;
			const bool coherent = source.Read(&values, &sequence);
			if (coherent && sequence != 0u)
				*valid_mask |= section_bit;
			else if (!coherent)
				*incoherent_mask |= section_bit;
			builder->WriteSection(kind, sequence, coherent,
				values.data(), values.size());
		}

		bool BuildRecord(u8* output, u64 journal_sequence,
			u64 run_id, u64 timestamp_us, bool live = false)
		{
			std::array<u8, RecordSize> record{};
			RecordBuilder builder{record.data()};
			u32 valid_mask = 0;
			u32 incoherent_mask = 0;
			AppendSection(&builder, SectionKind::GxmCall, s_gxm,
				1u << 0, &valid_mask, &incoherent_mask);
			AppendSection(&builder, SectionKind::Mtgs, s_mtgs,
				1u << 1, &valid_mask, &incoherent_mask);
			AppendSection(&builder, SectionKind::Retirement, s_retirement,
				1u << 2, &valid_mask, &incoherent_mask);
			AppendSection(&builder, SectionKind::Notification, s_notification,
				1u << 3, &valid_mask, &incoherent_mask);
			AppendSection(&builder, SectionKind::Cpu1, s_cpu1,
				1u << 4, &valid_mask, &incoherent_mask);
			AppendSection(&builder, SectionKind::Lifecycle, s_lifecycle,
				1u << 5, &valid_mask, &incoherent_mask);
			AppendSection(&builder, SectionKind::Cpu0, s_cpu0,
				1u << 6, &valid_mask, &incoherent_mask);
			std::array<u64, HandoffValueCount> handoff{};
			for (size_t i = 0; i < handoff.size(); i++)
				handoff[i] = s_handoff[i].load(std::memory_order_relaxed);
			builder.WriteSection(SectionKind::Handoff, 0, false,
				handoff.data(), handoff.size());
			// Encode only the last complete query sweep. The live observer never
			// calls a queried owner or enters the persistence thread's kernel APIs.
			// The sweep timestamp can be older than this record and must not be
			// interpreted as a current CPU-utilization sample.
			std::array<u64, ThreadValueCount> threads{};
			u32 thread_sequence = 0;
			(void)s_thread_activity.Read(&threads, &thread_sequence);
			builder.WriteSection(SectionKind::ThreadActivity, thread_sequence, false,
				threads.data(), threads.size());
			AppendSection(&builder, SectionKind::Writer, s_writer_activity,
				1u << 7, &valid_mask, &incoherent_mask);
			AppendSection(&builder, SectionKind::Probe, s_probe_activity,
				1u << 8, &valid_mask, &incoherent_mask);
			std::array<u64, NoOutputValueCount> no_output{};
			for (size_t i = 0; i < no_output.size(); i++)
				no_output[i] = s_no_output_executions[i].load(std::memory_order_relaxed);
			builder.WriteSection(SectionKind::NoOutputExecutions, 0, false,
				no_output.data(), no_output.size());
			if (!builder.okay || builder.offset > RecordSize)
				return false;

			WriteLe32(record.data(), live ? 0x4c485647u : RecordMagic); // GVHL / GVHJ
			WriteLe16(record.data() + 4u, RecordVersion);
			WriteLe16(record.data() + 6u, static_cast<u16>(HeaderSize));
			WriteLe32(record.data() + 8u, static_cast<u32>(RecordSize));
			WriteLe32(record.data() + 12u,
				static_cast<u32>(builder.offset - HeaderSize));
			WriteLe64(record.data() + 16u, journal_sequence);
			WriteLe64(record.data() + 24u, run_id);
			WriteLe64(record.data() + 32u, timestamp_us);
			WriteLe32(record.data() + 40u, valid_mask);
			WriteLe32(record.data() + 44u, incoherent_mask);
			WriteLe32(record.data() + 48u,
				s_open_failures.load(std::memory_order_relaxed));
			WriteLe32(record.data() + 52u,
				s_write_failures.load(std::memory_order_relaxed));
			WriteLe32(record.data() + 56u,
				s_sync_failures.load(std::memory_order_relaxed));
			WriteLe32(record.data() + CrcOffset, 0u);
			WriteLe32(record.data() + CrcOffset,
				CalculateCrc32(record.data(), record.size()));
			std::memcpy(output, record.data(), record.size());
			return true;
		}

#if defined(__vita__)
		void BeginWriterOperation(WriterOperation operation, u32 target = 0)
		{
			s_writer_state.update_time_us = sceKernelGetProcessTimeWide();
			s_writer_state.entered_time_us = s_writer_state.update_time_us;
			s_writer_state.returned_time_us = 0;
			s_writer_state.operation = static_cast<u32>(operation);
			s_writer_state.active = 1;
			s_writer_state.target = target;
			s_writer_state.result = 0;
			PublishWriterActivity(s_writer_state);
		}

		void EndWriterOperation(s32 result)
		{
			s_writer_state.update_time_us = sceKernelGetProcessTimeWide();
			s_writer_state.returned_time_us = s_writer_state.update_time_us;
			s_writer_state.active = 0;
			s_writer_state.result = result;
			PublishWriterActivity(s_writer_state);
		}

		void SampleThreads()
		{
			// Sony SceKernelThreadInfo::runClocks is cumulative microseconds;
			// common/Vita/VitaThreads.cpp::GetVitaThreadCpuTime owns this contract.
			std::array<u64, ThreadValueCount> threads{};
			threads[0] = sceKernelGetProcessTimeWide();
			for (size_t owner = 0; owner < s_thread_ids.size(); owner++)
			{
				const u32 id = s_thread_ids[owner].load(std::memory_order_relaxed);
				const size_t base = 1u + owner * 8u;
				threads[base] = id;
				SceKernelThreadInfo info{};
				info.size = sizeof(info);
				BeginWriterOperation(WriterOperation::QueryThread, id);
				const s32 result = id ? sceKernelGetThreadInfo(id, &info) : -1;
				EndWriterOperation(result);
				threads[base + 1u] = static_cast<u32>(result);
				if (result < 0)
					continue;
				threads[base + 2u] = static_cast<u64>(info.runClocks);
				threads[base + 3u] = info.status;
				threads[base + 4u] = info.waitType;
				threads[base + 5u] = static_cast<u32>(info.waitId);
				threads[base + 6u] = static_cast<u32>(info.currentPriority);
				threads[base + 7u] = static_cast<u32>(info.currentCpuId);
			}
			s_thread_activity.Publish(threads);
		}

		u64 ReadNewestSequence(SceUID fd)
		{
			u64 newest = 0;
			for (size_t slot = 0; slot < RecordCount; slot++)
			{
				BeginWriterOperation(WriterOperation::ReadPrevious, static_cast<u32>(slot));
				const int read = sceIoPread(fd, s_record_buffer.data(),
					static_cast<SceSize>(s_record_buffer.size()),
					static_cast<SceOff>(slot * RecordSize));
				EndWriterOperation(read);
				u64 sequence = 0;
				if (read == static_cast<int>(RecordSize) &&
					ValidateRecord(s_record_buffer.data(),
						s_record_buffer.size(), &sequence))
				{
					newest = std::max(newest, sequence);
				}
			}
			return newest;
		}

		bool WriteRecord(SceUID fd, u64 sequence, u64 flush_request)
		{
			s_writer_state.attempted_sequence = sequence;
			SampleThreads();
			BeginWriterOperation(WriterOperation::Encode);
			const bool encoded = BuildRecord(s_record_buffer.data(), sequence,
				s_run_id.load(std::memory_order_relaxed),
				sceKernelGetProcessTimeWide());
			EndWriterOperation(encoded ? 0 : -1);
			if (!encoded)
			{
				s_write_failures.fetch_add(1u, std::memory_order_relaxed);
				return false;
			}
			const size_t slot = static_cast<size_t>(sequence % RecordCount);
			BeginWriterOperation(WriterOperation::Write, static_cast<u32>(fd));
			const int written = sceIoPwrite(fd, s_record_buffer.data(),
				static_cast<SceSize>(s_record_buffer.size()),
				static_cast<SceOff>(slot * RecordSize));
			EndWriterOperation(written);
			if (written != static_cast<int>(RecordSize))
			{
				s_write_failures.fetch_add(1u, std::memory_order_relaxed);
				return false;
			}
			BeginWriterOperation(WriterOperation::Sync, static_cast<u32>(fd));
			const int synchronized = sceIoSyncByFd(fd, 0);
			if (synchronized >= 0)
			{
				s_writer_state.durable_sequence = sequence;
				s_writer_state.durable_time_us = sceKernelGetProcessTimeWide();
			}
			EndWriterOperation(synchronized);
			if (synchronized < 0)
			{
				s_sync_failures.fetch_add(1u, std::memory_order_relaxed);
				return false;
			}
			s_last_sequence.store(sequence, std::memory_order_release);
			s_flush_completed.store(flush_request, std::memory_order_release);
			return true;
		}

		void WaitForNextRecord()
		{
			BeginWriterOperation(WriterOperation::Sleep);
			// Check a fatal-path request at a 50 ms cadence without turning the
			// ordinary one-record-per-second observer into a filesystem workload.
			// No GPU/CPU owner posts a semaphore or performs another kernel call.
			for (u32 interval = 0u; interval < 20u; interval++)
			{
				if (s_writer_shutdown.load(std::memory_order_acquire) ||
					s_flush_requested.load(std::memory_order_acquire) !=
						s_flush_completed.load(std::memory_order_acquire))
				{
					break;
				}
				Threading::Sleep(50);
			}
			EndWriterOperation(0);
		}

		void WriterThreadMain()
		{
			RegisterOwnerThread(4);
			Threading::SetNameOfCurrentThread("GPU-VU journal");
			const Threading::ThreadHandle self =
				Threading::ThreadHandle::GetForCallingThread();
			const bool affinity_set = self.SetAffinity(1u << 2);
			const int pinned_priority = sceKernelGetThreadCurrentPriority();
			int priority_result = -1;
			if (affinity_set && pinned_priority >= 64 && pinned_priority <= 127)
			{
				// The writer shares USER_2 with MTGS. Keep this normally sleeping
				// observer one individual-queue step above MTGS so a
				// continuously runnable GXM/MTGS spin cannot starve durable evidence.
				const int writer_priority = pinned_priority > 64 ?
					pinned_priority - 1 : pinned_priority;
				priority_result = sceKernelChangeThreadPriority(
					sceKernelGetThreadId(), writer_priority);
			}
			const int effective_priority = sceKernelGetThreadCurrentPriority();
			const bool scheduling_ready = affinity_set && priority_result >= 0 &&
				effective_priority >= 64 && effective_priority <= 127;
			if (!scheduling_ready)
			{
				s_writer_startup_result.store(2u, std::memory_order_release);
				s_writer_startup_sema.Post();
				return;
			}
			SceUID fd = -1;
			u64 sequence = 0;
			bool startup_pending = true;
			while (!s_writer_shutdown.load(std::memory_order_acquire))
			{
				if (fd < 0)
				{
					BeginWriterOperation(WriterOperation::Open);
					fd = sceIoOpen(FilePath, SCE_O_RDWR | SCE_O_CREAT, 0666);
					EndWriterOperation(fd);
					if (fd < 0)
					{
						s_open_failures.fetch_add(1u,
							std::memory_order_relaxed);
						if (startup_pending)
						{
							s_writer_startup_result.store(2u, std::memory_order_release);
							s_writer_startup_sema.Post();
							return;
						}
						WaitForNextRecord();
						continue;
					}
					sequence = ReadNewestSequence(fd);
					s_last_sequence.store(sequence,
						std::memory_order_relaxed);
					if (s_run_id.load(std::memory_order_relaxed) == 0u)
					{
						// Fold the durable predecessor sequence into the launch
						// identity. Process time alone starts near the same value on
						// every title launch and is not a cross-reboot identifier.
						const u64 process_time = sceKernelGetProcessTimeWide();
						u64 generated_run_id =
							(sequence + 1u) * 0x9e3779b97f4a7c15ull ^
							(process_time + 0xd1b54a32d192ed03ull);
						if (generated_run_id == 0u)
							generated_run_id = 1u;
						s_run_id.store(generated_run_id,
							std::memory_order_relaxed);
					}
				}

				const u64 flush_request =
					s_flush_requested.load(std::memory_order_acquire);
				if (!WriteRecord(fd, ++sequence, flush_request))
				{
					BeginWriterOperation(WriterOperation::Close, static_cast<u32>(fd));
					EndWriterOperation(sceIoClose(fd));
					fd = -1;
					if (startup_pending)
					{
						s_writer_startup_result.store(2u, std::memory_order_release);
						s_writer_startup_sema.Post();
						return;
					}
				}
				else if (startup_pending)
				{
					// Admission requires evidence from this launch on storage, not
					// merely a scheduled writer or a previous launch's valid record.
					startup_pending = false;
					s_writer_running.store(1u, std::memory_order_release);
					s_writer_startup_result.store(1u, std::memory_order_release);
					s_writer_startup_sema.Post();
				}
				WaitForNextRecord();
			}

			if (fd >= 0)
			{
				(void)WriteRecord(fd, ++sequence,
					s_flush_requested.load(std::memory_order_acquire));
				BeginWriterOperation(WriterOperation::Close, static_cast<u32>(fd));
				EndWriterOperation(sceIoClose(fd));
			}
			s_writer_running.store(0u, std::memory_order_release);
		}

		void BeginProbeOperation(ProbeOperation operation)
		{
			s_probe_state.update_time_us = sceKernelGetProcessTimeWide();
			s_probe_state.entered_time_us = s_probe_state.update_time_us;
			s_probe_state.returned_time_us = 0;
			s_probe_state.operation = static_cast<u32>(operation);
			s_probe_state.active = 1;
			s_probe_state.result = 0;
			s_probe_state.error_number = 0;
			PublishProbeActivity(s_probe_state);
		}

		int EndProbeOperation(int result, bool network = false)
		{
			// Capture libnet's thread-local errno immediately, before another call.
			s_probe_state.error_number = network && result < 0 ? *sceNetErrnoLoc() : 0;
			s_probe_state.update_time_us = sceKernelGetProcessTimeWide();
			s_probe_state.returned_time_us = s_probe_state.update_time_us;
			s_probe_state.active = 0;
			s_probe_state.result = result;
			PublishProbeActivity(s_probe_state);
			return result;
		}

		void LiveThreadMain()
		{
			Threading::SetNameOfCurrentThread("GPU-VU live");
			const auto self = Threading::ThreadHandle::GetForCallingThread();
			// A separate application core from persistence/MTGS, one individual
			// priority step above ordinary work. No CPU3/system scheduling changes.
			BeginProbeOperation(ProbeOperation::Affinity);
			if (EndProbeOperation(self.SetAffinity(1u << 0) ? 0 : -1) < 0)
				return;
			BeginProbeOperation(ProbeOperation::Priority);
			const int priority = sceKernelGetThreadCurrentPriority();
			if (EndProbeOperation(priority < 65 || priority > 127 ? -1 :
				sceKernelChangeThreadPriority(sceKernelGetThreadId(), priority - 1)) < 0)
			{
				return;
			}
			BeginProbeOperation(ProbeOperation::LoadModule);
			const bool load_module = sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != 0;
			if (EndProbeOperation(load_module ? sceSysmoduleLoadModule(SCE_SYSMODULE_NET) : 0) < 0)
				return;
			// Sony libnet reference: SceNetInitParam recommends >=16 KiB for
			// infrastructure; retain a bounded 64 KiB process-lifetime pool.
			alignas(64) static std::array<u8, 64u * 1024u> network_memory{};
			SceNetInitParam init{network_memory.data(), static_cast<int>(network_memory.size()), 0};
			BeginProbeOperation(ProbeOperation::Init);
			// sceNetInit returns its error directly; Sony marks sce_net_errno
			// invalid for this API (unlike the socket calls below).
			const int initialized = EndProbeOperation(sceNetInit(&init));
			const bool own_net = initialized >= 0;
			int socket = -1;
			if (own_net || initialized == SCE_NET_ERROR_EBUSY)
			{
				BeginProbeOperation(ProbeOperation::Socket);
				socket = EndProbeOperation(sceNetSocket("GPU-VU live", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0), true);
			}
			const int nonblocking = 1;
			SceNetSockaddrIn local{};
			local.sin_len = sizeof(local);
			local.sin_family = SCE_NET_AF_INET;
			local.sin_port = sceNetHtons(static_cast<u16>(s_live_port));
			local.sin_addr.s_addr = SCE_NET_INADDR_ANY;
			bool ready = false;
			if (socket >= 0)
			{
				BeginProbeOperation(ProbeOperation::Nonblocking);
				if (EndProbeOperation(sceNetSetsockopt(socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO,
					&nonblocking, sizeof(nonblocking)), true) >= 0)
				{
					BeginProbeOperation(ProbeOperation::Bind);
					ready = EndProbeOperation(sceNetBind(socket,
						reinterpret_cast<const SceNetSockaddr*>(&local), sizeof(local)), true) >= 0;
				}
			}
			sceClibPrintf("GPU-VU live probe: ready=%u port=%u init=0x%08x socket=%d\n",
				ready ? 1u : 0u, s_live_port, initialized, socket);
			if (ready)
			{
				std::array<u8, RecordSize> record{};
				u64 sequence = 0;
				while (!s_live_shutdown.load(std::memory_order_acquire))
				{
					// WSL2 initiates the exchange; replying to the observed sender
					// keeps NAT traversal on that socket without host configuration.
					// Bound malformed-packet work and send at most one reply/second.
					for (u32 attempt = 0; attempt < 4u; attempt++)
					{
						std::array<u8, LiveProbeRequestSize + 1u> request{};
						SceNetSockaddrIn peer{};
						unsigned int peer_size = sizeof(peer);
						BeginProbeOperation(ProbeOperation::Receive);
						const int received = EndProbeOperation(sceNetRecvfrom(socket, request.data(), request.size(),
							SCE_NET_MSG_DONTWAIT, reinterpret_cast<SceNetSockaddr*>(&peer), &peer_size), true);
						if (received < 0)
							break;
						s_probe_state.received_requests++;
						if (peer_size != sizeof(peer) || peer.sin_family != SCE_NET_AF_INET ||
							!ValidateLiveProbeRequest(request.data(), static_cast<size_t>(received)))
						{
							s_probe_state.rejected_requests++;
							continue;
						}
						BeginProbeOperation(ProbeOperation::Encode);
						if (EndProbeOperation(BuildLiveRecord(record.data(), record.size(), ++sequence,
							sceKernelGetProcessTimeWide()) ? 0 : -1) >= 0)
						{
							// Sony libnet DONTWAIT: no wait for receive/send buffer
							// space. No resend, application queue, DNS or recovery.
							s_probe_state.attempted_replies++;
							BeginProbeOperation(ProbeOperation::Send);
							if (EndProbeOperation(sceNetSendto(socket, record.data(), record.size(), SCE_NET_MSG_DONTWAIT,
								reinterpret_cast<const SceNetSockaddr*>(&peer), sizeof(peer)), true) == RecordSize)
							{
								s_probe_state.sent_replies++;
								s_probe_state.last_reply_time_us = s_probe_state.returned_time_us;
							}
						}
						break;
					}
					// Keep the last network result visible while sleeping, including
					// expected EAGAIN. A sleep breadcrumb would hide that evidence.
					PublishProbeActivity(s_probe_state);
					for (u32 interval = 0; interval < 20 &&
						!s_live_shutdown.load(std::memory_order_acquire); interval++)
					{
						Threading::Sleep(50);
					}
				}
			}
			// Cleanup must not overwrite the stage/result explaining startup failure.
			if (socket >= 0)
				(void)sceNetSocketClose(socket);
			if (own_net)
				(void)sceNetTerm();
			if (load_module)
				(void)sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
		}
#endif
	} // namespace

	void PublishGxmCall(const GxmCallState& state)
	{
		std::array<u64, GxmValueCount> values{};
		size_t index = 0;
		values[index++] = state.update_time_us;
		values[index++] = state.entered_time_us;
		values[index++] = state.returned_time_us;
		values[index++] = state.token;
		values[index++] = state.sequence_begin;
		values[index++] = state.sequence_end;
		values[index++] = state.scene;
		values[index++] = state.program_key_high;
		values[index++] = state.program_key_low;
		values[index++] = state.input_owner;
		values[index++] = state.output_address;
		values[index++] = state.notification_address;
		values[index++] = state.buffer0_address;
		values[index++] = state.buffer1_address;
		values[index++] = state.buffer2_address;
		values[index++] = state.buffer4_address;
		values[index++] = state.index_address;
		values[index++] = static_cast<u32>(state.result);
		values[index++] = state.active;
		values[index++] = state.kind;
		values[index++] = state.submission_stage;
		values[index++] = state.program_abi;
		values[index++] = state.retirement_slot;
		values[index++] = state.input_slot;
		values[index++] = state.input_generation;
		values[index++] = state.input_first_qword;
		values[index++] = state.input_last_qword;
		values[index++] = state.object_count;
		values[index++] = state.index_count;
		values[index++] = state.private_transaction_count;
		values[index++] = state.output_bytes;
		values[index++] = state.output_maximum_write_word;
		values[index++] = state.notification_required;
		values[index++] = state.notification_observed;
		values[index++] = state.flags;
		s_gxm.Publish(values);
	}

	void PublishCpu0(const Cpu0State& state)
	{
		RegisterOwnerThread(0);
		s_cpu0.Publish({state.update_time_us, state.stage_started_time_us,
			state.wait_visits, state.execute_enqueued, state.execute_completed,
			state.stage, state.ring_read, state.ring_write, state.requested_words,
			state.queued_frames, state.vu_read, state.vu_published_write,
			state.vu_private_write, state.pending_vif_batch});
	}

	void PublishWriterActivity(const WriterActivity& state)
	{
		s_writer_activity.Publish({state.update_time_us, state.entered_time_us,
			state.returned_time_us, state.attempted_sequence, state.durable_sequence,
			state.durable_time_us, state.operation, state.active, state.target,
			static_cast<u32>(state.result)});
	}

	void PublishProbeActivity(const ProbeActivity& state)
	{
		s_probe_activity.Publish({state.update_time_us, state.entered_time_us,
			state.returned_time_us, state.received_requests, state.rejected_requests,
			state.attempted_replies, state.sent_replies, state.last_reply_time_us,
			state.operation, state.active, static_cast<u32>(state.result),
			static_cast<u32>(state.error_number)});
	}

	bool ConfigureLiveProbeBeforeVmStart(const char* token_hex, u32 port)
	{
#if defined(__vita__)
		if (s_writer_thread.Joinable() || s_live_thread.Joinable())
			return false;
#endif
		s_live_token.fill(0);
		s_live_port = 0;
		if (!token_hex || !*token_hex)
			return true;
		if (port == 0 || port > 65535 || std::strlen(token_hex) != s_live_token.size() * 2u)
			return false;
		u32 nonzero = 0;
		for (size_t index = 0; index < s_live_token.size() * 2u; index++)
		{
			const char ch = token_hex[index];
			const u32 value = ch >= '0' && ch <= '9' ? ch - '0' :
				ch >= 'a' && ch <= 'f' ? ch - 'a' + 10u :
				ch >= 'A' && ch <= 'F' ? ch - 'A' + 10u : 16u;
			if (value > 15u)
				return false;
			s_live_token[index / 2u] |= static_cast<u8>(value << (index % 2u ? 0u : 4u));
			nonzero |= value;
		}
		if (!nonzero)
			return false;
		s_live_port = port;
		return true;
	}

	bool ValidateLiveProbeRequest(const u8* request, size_t size)
	{
		if (!s_live_port || !request || size != LiveProbeRequestSize ||
			std::memcmp(request, "GVHP", 4u) != 0)
		{
			return false;
		}
		// Plaintext bearer token gates replies, not encrypted transport or
		// authenticated observations. Compare all bytes without early exits.
		u32 difference = 0;
		for (size_t index = 0; index < s_live_token.size(); index++)
			difference |= request[4u + index] ^ s_live_token[index];
		return difference == 0;
	}

	void AddHandoffCounter(HandoffCounter counter, u32 count)
	{
		const size_t index = static_cast<size_t>(counter);
		if (index < s_handoff.size())
			s_handoff[index].fetch_add(count, std::memory_order_relaxed);
	}

	void CountNoOutputExecution(NoOutputExecutionKind kind)
	{
		const size_t index = static_cast<size_t>(kind);
		if (index < s_no_output_executions.size())
			s_no_output_executions[index].fetch_add(1u, std::memory_order_relaxed);
	}

	void PublishMtgs(const MtgsState& state)
	{
		RegisterOwnerThread(2);
		std::array<u64, MtgsValueCount> values{};
		size_t index = 0;
		values[index++] = state.update_time_us;
		values[index++] = state.stage_started_time_us;
		values[index++] = state.ticket;
		values[index++] = state.sequence;
		values[index++] = state.scene;
		values[index++] = state.last_completion_sequence;
		values[index++] = state.stage;
		values[index++] = state.ring_read_position;
		values[index++] = state.ring_write_position;
		values[index++] = state.current_command;
		values[index++] = state.reservations_remaining;
		values[index++] = state.completion_read_position;
		values[index++] = state.completion_write_position;
		values[index++] = state.completion_pending;
		values[index++] = state.completion_logical_count;
		values[index++] = state.completion_physical_count;
		values[index++] = state.submission_marker_result;
		values[index++] = state.wake_reason_flags;
		values[index++] = state.flags;
		s_mtgs.Publish(values);
	}

	void PublishRetirement(const RetirementState& state)
	{
		std::array<u64, RetirementValueCount> values{};
		size_t index = 0;
		values[index++] = state.update_time_us;
		values[index++] = state.stage;
		values[index++] = state.current_slot;
		values[index++] = state.action;
		values[index++] = state.pass;
		for (const RetirementSlotState& slot : state.slots)
		{
			values[index++] = slot.update_time_us;
			values[index++] = slot.sequence_begin;
			values[index++] = slot.sequence_end;
			values[index++] = slot.scene;
			values[index++] = slot.notification_address;
			values[index++] = slot.submitted;
			values[index++] = slot.stage;
			values[index++] = slot.notification_required;
			values[index++] = slot.notification_observed;
			values[index++] = slot.draw_count;
			values[index++] = slot.private_output_count;
			values[index++] = slot.input_retention_count;
			values[index++] = slot.allocation_count;
			values[index++] = slot.flags;
		}
		s_retirement.Publish(values);
	}

	void PublishNotification(const NotificationState& state)
	{
		RegisterOwnerThread(3);
		std::array<u64, NotificationValueCount> values{};
		size_t index = 0;
		values[index++] = state.update_time_us;
		values[index++] = state.active_address;
		values[index++] = state.active_sequence;
		values[index++] = state.pending_address;
		values[index++] = state.pending_sequence;
		values[index++] = state.arm_count;
		values[index++] = state.poll_count;
		values[index++] = state.wake_count;
		values[index++] = state.deferral_count;
		values[index++] = state.stage;
		values[index++] = state.request;
		values[index++] = state.completed;
		values[index++] = state.active_required;
		values[index++] = state.active_observed;
		values[index++] = state.pending_required;
		values[index++] = state.pending_observed;
		values[index++] = state.flags;
		values[index++] = 0;
		s_notification.Publish(values);
	}

	void PublishCpu1(const Cpu1State& state)
	{
		RegisterOwnerThread(1);
		std::array<u64, Cpu1ValueCount> values{};
		size_t index = 0;
		values[index++] = state.update_time_us;
		values[index++] = state.sequence_front;
		values[index++] = state.sequence_back;
		values[index++] = state.private_generation;
		values[index++] = state.stage;
		values[index++] = state.pending_physical_count;
		values[index++] = state.pending_logical_count;
		values[index++] = state.private_transaction_count;
		values[index++] = state.private_generation_valid;
		values[index++] = state.wait_poll_count;
		values[index++] = state.flags;
		s_cpu1.Publish(values);
	}

	void PublishLifecycle(const LifecycleState& state)
	{
		std::array<u64, LifecycleValueCount> values{};
		size_t index = 0;
		values[index++] = state.update_time_us;
		values[index++] = state.suspend_count;
		values[index++] = state.resume_count;
		values[index++] = state.overlay_enter_count;
		values[index++] = state.overlay_leave_count;
		values[index++] = state.stage;
		values[index++] = state.system_ui_overlaid;
		values[index++] = state.app_state;
		values[index++] = state.flags;
		s_lifecycle.Publish(values);
	}

	bool BuildRecordForValidation(u8* output, size_t output_size,
		u64 journal_sequence, u64 run_id, u64 timestamp_us)
	{
		if (!output || output_size < RecordSize)
			return false;
		return BuildRecord(output, journal_sequence, run_id, timestamp_us);
	}

	bool BuildLiveRecord(u8* output, size_t output_size,
		u64 stream_sequence, u64 timestamp_us)
	{
		if (!output || output_size < RecordSize)
			return false;
		return BuildRecord(output, stream_sequence,
			s_run_id.load(std::memory_order_relaxed), timestamp_us, true);
	}

	bool ValidateRecord(const u8* record, size_t record_size,
		u64* journal_sequence)
	{
		if (!record || record_size < RecordSize ||
			ReadLe32(record) != RecordMagic ||
			ReadLe16(record + 4u) != RecordVersion ||
			ReadLe16(record + 6u) != HeaderSize ||
			ReadLe32(record + 8u) != RecordSize)
		{
			return false;
		}
		const u32 payload_size = ReadLe32(record + 12u);
		if (payload_size > RecordSize - HeaderSize)
			return false;

		std::array<u8, RecordSize> copy{};
		std::memcpy(copy.data(), record, copy.size());
		const u32 expected_crc = ReadLe32(copy.data() + CrcOffset);
		WriteLe32(copy.data() + CrcOffset, 0u);
		if (CalculateCrc32(copy.data(), copy.size()) != expected_crc)
			return false;
		if (journal_sequence)
			*journal_sequence = ReadLe64(record + 16u);
		return true;
	}

	bool Initialize(u64 run_id)
	{
#if defined(__vita__)
		if (s_writer_thread.Joinable())
			return IsRunning();
#endif
		// Initialize runs before the notification/MTGS owners are admitted. Clear
		// a prior in-process launch so its last coherent sections cannot appear in
		// the first record of a reopened renderer.
		s_gxm.Reset();
		s_mtgs.Reset();
		s_retirement.Reset();
		s_notification.Reset();
		s_cpu1.Reset();
		s_lifecycle.Reset();
		s_cpu0.Reset();
		s_writer_activity.Reset();
		s_probe_activity.Reset();
		s_thread_activity.Reset();
		for (auto& count : s_handoff)
			count.store(0, std::memory_order_relaxed);
		for (auto& count : s_no_output_executions)
			count.store(0, std::memory_order_relaxed);
		for (auto& id : s_thread_ids)
			id.store(0, std::memory_order_relaxed);
#if defined(__vita__)
		s_run_id.store(run_id, std::memory_order_relaxed);
		s_last_sequence.store(0u, std::memory_order_relaxed);
		s_open_failures.store(0u, std::memory_order_relaxed);
		s_write_failures.store(0u, std::memory_order_relaxed);
		s_sync_failures.store(0u, std::memory_order_relaxed);
		s_flush_requested.store(0u, std::memory_order_relaxed);
		s_flush_completed.store(0u, std::memory_order_relaxed);
		s_writer_running.store(0u, std::memory_order_relaxed);
		s_writer_startup_result.store(0u, std::memory_order_relaxed);
		s_writer_shutdown.store(false, std::memory_order_relaxed);
		s_writer_state = {};
		s_writer_thread.SetStackSize(32u * 1024u);
		if (!s_writer_thread.Start(&WriterThreadMain))
			return false;
		s_writer_startup_sema.Wait();
		if (s_writer_startup_result.load(std::memory_order_acquire) != 1u)
		{
			s_writer_thread.Join();
			return false;
		}
		if (s_live_port != 0)
		{
			s_live_shutdown.store(false, std::memory_order_relaxed);
			s_live_thread.SetStackSize(32u * 1024u);
			s_probe_state = {};
			BeginProbeOperation(ProbeOperation::Start);
			// Transfer publication ownership to the thread only on success.
			if (!s_live_thread.Start(&LiveThreadMain))
				(void)EndProbeOperation(-1);
		}
		return true;
#else
		s_run_id.store(run_id, std::memory_order_relaxed);
		return true;
#endif
	}

	void Shutdown()
	{
#if defined(__vita__)
		if (s_live_thread.Joinable())
		{
			s_live_shutdown.store(true, std::memory_order_release);
			s_live_thread.Join();
		}
		if (!s_writer_thread.Joinable())
			return;
		s_writer_shutdown.store(true, std::memory_order_release);
		s_writer_thread.Join();
#endif
	}

	bool IsRunning()
	{
		return s_writer_running.load(std::memory_order_acquire) != 0u;
	}

	WriterStatus GetWriterStatus()
	{
		WriterStatus status;
		status.last_sequence = s_last_sequence.load(std::memory_order_acquire);
		status.run_id = s_run_id.load(std::memory_order_relaxed);
		status.running = s_writer_running.load(std::memory_order_acquire);
		status.open_failures = s_open_failures.load(std::memory_order_relaxed);
		status.write_failures = s_write_failures.load(std::memory_order_relaxed);
		status.sync_failures = s_sync_failures.load(std::memory_order_relaxed);
		return status;
	}

	bool FlushForFatal(u32 timeout_us)
	{
#if defined(__vita__)
		if (s_writer_running.load(std::memory_order_acquire) == 0u)
			return false;
		const u64 request = s_flush_requested.fetch_add(
			1u, std::memory_order_acq_rel) + 1u;
		for (u32 waited_us = 0u; waited_us < timeout_us; waited_us += 1000u)
		{
			if (s_flush_completed.load(std::memory_order_acquire) >= request)
				return true;
			sceKernelDelayThread(1000u);
		}
		return s_flush_completed.load(std::memory_order_acquire) >= request;
#else
		(void)timeout_us;
		return false;
#endif
	}
} // namespace VitaGpuVu::HealthJournal
