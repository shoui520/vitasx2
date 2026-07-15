cmake_minimum_required(VERSION 3.19)

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

option(VITASX2_PRODUCT_BOOT_VALIDATION
	"Stop the normal VitaSX2 product after bounded reset-to-entry and later-workload oracle gates."
	OFF)
if(VITASX2_PRODUCT_BOOT_VALIDATION)
	# The IOP provider's bounded fallback sentinels live in pcsx2core. Keep them
	# out of the normal product hot path, but compile them into this validation
	# configuration as well as the portable-replay configuration.
	add_compile_definitions(VITASX2_PRODUCT_BOOT_VALIDATION=1)
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
