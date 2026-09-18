cmake_minimum_required(VERSION 3.19)

if(POLICY CMP0118)
	# GXM shader objects are generated in pcsx2/ and consumed by the final
	# executable in vita/. Keep GENERATED visible across directory scopes.
	cmake_policy(SET CMP0118 NEW)
endif()

# VitaSX2 targets only the PlayStation Vita. Default to the VitaSDK toolchain
# so a plain `cmake -B build` does the right thing.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
	if(DEFINED ENV{VITASDK})
		set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
			CACHE PATH "toolchain file")
	else()
		message(FATAL_ERROR
			"VITASDK is not set and no toolchain file was given. VitaSX2 only builds for the PlayStation Vita.")
	endif()
endif()

project(vitasx2 LANGUAGES C CXX ASM)

if(NOT VITA)
	message(FATAL_ERROR
		"VitaSX2 only targets the PlayStation Vita (arm-vita-eabi).")
endif()

include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "build type" FORCE)
endif()

# Generated GPU-VU admission is the playable Vita product path. This switch
# retains the CPU MTVU control without importing development-only targets.
option(VITASX2_GPU_VU_DIRECT_ADMISSION
	"Use generated affine VU1+TFX draws instead of the CPU MTVU control."
	ON)
if(VITASX2_GPU_VU_DIRECT_ADMISSION)
	add_compile_definitions(VITASX2_GPU_VU_DIRECT_ADMISSION=1)
endif()

# Target CPU contract: Cortex-A9 MPCore, VFPv3, NEON, hard-float.
add_compile_options(
	-mcpu=cortex-a9
	-mfpu=neon
	-mfloat-abi=hard
	-ffunction-sections
	-fdata-sections
	-fno-strict-aliasing
	-Wno-psabi
)

# Vita user pages are 4 KiB; Cortex-A9 L1/L2 cache lines are 32 bytes.
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
