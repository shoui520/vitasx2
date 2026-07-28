cmake_minimum_required(VERSION 3.19)

if(POLICY CMP0118)
	# GXM shader objects are generated in pcsx2/ and consumed as INTERFACE
	# sources by final executables in vita/. Keep GENERATED visible across those
	# directory scopes on clean builds.
	cmake_policy(SET CMP0118 NEW)
endif()

# VitaSX2 targets only the PlayStation Vita. Default to the VitaSDK toolchain
# so a plain `cmake -B build` does the right thing.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
	if(DEFINED ENV{VITASDK})
		set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
	else()
		message(FATAL_ERROR "VITASDK is not set and no toolchain file was given. VitaSX2 only builds for the PlayStation Vita.")
	endif()
endif()

project(vitasx2 LANGUAGES C CXX ASM)

if(NOT VITA)
	message(FATAL_ERROR "VitaSX2 only targets the PlayStation Vita (arm-vita-eabi).")
endif()

include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "build type" FORCE)
endif()

option(VITASX2_PORTABLE_REPLAY_VALIDATION
	"Enable the bounded cross-core hooks required by portable replay validation."
	OFF)
if(VITASX2_PORTABLE_REPLAY_VALIDATION)
	add_compile_definitions(VITASX2_PORTABLE_REPLAY_VALIDATION=1)
endif()

option(VITASX2_VENEZIA_BENCHMARK
	"Build the finite VVNZRunner Venezia/NEON latency and throughput benchmark."
	OFF)
set(VITASX2_VENEZIA_SDK_ROOT "${PROJECT_SOURCE_DIR}/build/venezia-sdk" CACHE PATH
	"MeP SDK root containing bin/mep-elf-gcc and the Venezia target libraries.")
set(VITASX2_VVNZRUNNER_SOURCE_ROOT "${PROJECT_SOURCE_DIR}/../VVNZRunner" CACHE PATH
	"VVNZRunner source checkout used for its firmware floating-point helper contract.")

option(VITASX2_PRODUCT_BOOT_VALIDATION
	"Stop the normal VitaSX2 product after bounded reset-to-entry and later-workload oracle gates."
	OFF)
if(VITASX2_PRODUCT_BOOT_VALIDATION)
	# The IOP provider's bounded fallback sentinels live in pcsx2core. Keep them
	# out of the normal product hot path, but compile them into this validation
	# configuration as well as the portable-replay configuration.
	add_compile_definitions(VITASX2_PRODUCT_BOOT_VALIDATION=1)
endif()

option(VITASX2_CPU_PROFILER
	"Compile the opt-in sparse exclusive CPU0 stage profiler."
	OFF)
if(VITASX2_CPU_PROFILER)
	# Scheduler stage markers are compiled completely out of the playable
	# product. Profile builds use the CEX-supported process clock only on a
	# sparse subset of scheduler entries.
	add_compile_definitions(VITASX2_CPU_PROFILER=1)
endif()

option(VITASX2_VITA_SOFTWARE_GS_CONTROL
	"Use PCSX2's software GS renderer with the current Vita core and GXM presenter."
	OFF)
if(VITASX2_VITA_SOFTWARE_GS_CONTROL)
	# This is a bounded renderer-isolation control, not a second product mode.
	# GS.cpp::OpenGSRenderer() owns the SW/HW distinction; keep the definition
	# process-wide so GS state, texture-cache behavior, and the Vita mailbox agree.
	add_compile_definitions(VITASX2_VITA_SOFTWARE_GS_CONTROL=1)
endif()

option(VITASX2_GS_DRAW_TRACE
	"Record a bounded binary GSHWDrawConfig stream for PCSX2-oracle comparison."
	OFF)
set(VITASX2_GS_DRAW_TRACE_RECORDS "4096" CACHE STRING
	"Maximum GSHWDrawConfig records in an opt-in Vita GS draw trace.")
set(VITASX2_GS_DRAW_TRACE_SKIP_RECORDS "0" CACHE STRING
	"GSHWDrawConfig records to skip after ELF entry before tracing.")
set(VITASX2_GS_DRAW_TRACE_START_VSYNC "0" CACHE STRING
	"Do not record GS draws or worker packets before this VSync after ELF entry.")
set(VITASX2_GS_DRAW_TRACE_VSYNCS "600" CACHE STRING
	"Maximum VSyncs after ELF entry before closing an opt-in Vita GS draw trace.")
set(VITASX2_GS_DRAW_TRACE_RT_SNAPSHOTS "512" CACHE STRING
	"Maximum changed render-target source snapshots in an opt-in Vita GS draw trace.")
set(VITASX2_GS_DRAW_TRACE_RT_AFTER_DRAWS "0" CACHE STRING
	"Maximum post-draw render-target readbacks in an opt-in Vita GS draw trace.")
set(VITASX2_GS_DRAW_TRACE_PACKETS "0" CACHE STRING
	"Maximum raw GS worker packets in an opt-in Vita GS draw trace.")
set(VITASX2_GS_DRAW_TRACE_PRESENT_VSYNC "0" CACHE STRING
	"Capture the merged source and final display buffer at this opt-in trace VSync; zero disables capture.")
if(VITASX2_GS_DRAW_TRACE)
	# Compile the recorder into every translation unit which owns one side of
	# the GSRendererHW -> GSDeviceGXM boundary. The normal product has neither
	# these calls nor a per-draw enable branch.
	add_compile_definitions(
		VITASX2_GS_DRAW_TRACE=1
		VITASX2_GS_DRAW_TRACE_RECORDS=${VITASX2_GS_DRAW_TRACE_RECORDS}
		VITASX2_GS_DRAW_TRACE_SKIP_RECORDS=${VITASX2_GS_DRAW_TRACE_SKIP_RECORDS}
		VITASX2_GS_DRAW_TRACE_START_VSYNC=${VITASX2_GS_DRAW_TRACE_START_VSYNC}
		VITASX2_GS_DRAW_TRACE_VSYNCS=${VITASX2_GS_DRAW_TRACE_VSYNCS}
		VITASX2_GS_DRAW_TRACE_RT_SNAPSHOTS=${VITASX2_GS_DRAW_TRACE_RT_SNAPSHOTS}
		VITASX2_GS_DRAW_TRACE_RT_AFTER_DRAWS=${VITASX2_GS_DRAW_TRACE_RT_AFTER_DRAWS}
		VITASX2_GS_DRAW_TRACE_PACKETS=${VITASX2_GS_DRAW_TRACE_PACKETS}
		VITASX2_GS_DRAW_TRACE_PRESENT_VSYNC=${VITASX2_GS_DRAW_TRACE_PRESENT_VSYNC}
	)
endif()

option(VITASX2_GPU_VU_CAPTURE_WITH_CPU_REPLAY
	"Diagnostic control: copy affine VIF payloads to non-cacheable GXM memory even though the disconnected GPU-VU path immediately replays them on MTVU."
	OFF)
if(VITASX2_GPU_VU_CAPTURE_WITH_CPU_REPLAY)
	# This reproduces the pre-admission ownership boundary for matched hardware
	# controls. The product must not pay for a non-cacheable GPU copy until a
	# GpuVuDraw can actually retain and consume it.
	add_compile_definitions(VITASX2_GPU_VU_CAPTURE_WITH_CPU_REPLAY=1)
endif()

option(VITASX2_GPU_VU_DIRECT_ADMISSION
	"Use generated affine VU1+TFX draws; disable only for the Maximum Cortex-A9 fallback/control."
	ON)
if(VITASX2_GPU_VU_DIRECT_ADMISSION)
	# Keep this process-wide: VIF capture, MTVU ownership, the GS mailbox, and
	# GSDeviceGXM must agree whether immutable input spans can replace CPU VU
	# execution. Phase 1 hardware and SDK-sweep evidence makes this the playable
	# Vita default; an explicit OFF build retains the Maximum Cortex-A9 control
	# and compiles all disconnected capture work out.
	add_compile_definitions(VITASX2_GPU_VU_DIRECT_ADMISSION=1)
endif()
if(VITASX2_GPU_VU_CAPTURE_WITH_CPU_REPLAY AND
	VITASX2_GPU_VU_DIRECT_ADMISSION)
	message(FATAL_ERROR
		"VITASX2_GPU_VU_CAPTURE_WITH_CPU_REPLAY is a disconnected control; "
		"configure VITASX2_GPU_VU_DIRECT_ADMISSION=OFF with it.")
endif()

# Target CPU contract: Cortex-A9 MPCore, VFPv3, NEON, hard-float.
# NEON is mandatory (see docs/ps2-vita/GENERIC_CORTEX_A9_OPTIMIZATION.md).
add_compile_options(
	-mcpu=cortex-a9
	-mfpu=neon
	-mfloat-abi=hard
	-ffunction-sections
	-fdata-sections
	-fno-strict-aliasing
	-Wno-psabi
)

# Cortex-A9/Vita host constants consumed by common/Pcsx2Defs.h.
# Vita user pages are 4 KiB granularity; A9 L1/L2 cache lines are 32 bytes.
add_compile_definitions(
	_M_ARM32=1
	OVERRIDE_HOST_PAGE_SIZE=4096
	OVERRIDE_HOST_CACHE_LINE_SIZE=32
	__POSIX__=1
)

add_link_options(-Wl,--gc-sections)

add_subdirectory(3rdparty)
add_subdirectory(common)
add_subdirectory(pcsx2)
add_subdirectory(vita)
