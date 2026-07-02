# Vita-native layer: host runtime, probes, GXM backend, A32 emitters.

set(VITASX2_TITLEID "VSX200001")
set(VITASX2_CORE_SMOKE_TITLEID "VSX200002")
set(VITASX2_BIOS_SMOKE_TITLEID "VSX200003")
set(VITASX2_BIOS_EE_TRACE_TITLEID "VSX200004")
set(VITASX2_BIOS_IOP_TRACE_TITLEID "VSX200005")
set(VITASX2_CUBE_EE_TRACE_TITLEID "VSX200006")
set(VITASX2_SIMPLE2000_EE_TRACE_TITLEID "VSX200007")
set(VITASX2_A32_EMITTER_SMOKE_TITLEID "VSX200008")
set(VITASX2_A32_PROVIDER_SMOKE_TITLEID "VSX200009")

set(VITASX2_CUBE_EE_TRACE_RECORDS "4096" CACHE STRING "Cube EE trace records to write.")
set(VITASX2_CUBE_EE_TRACE_SKIP_RECORDS "0" CACHE STRING "Cube EE trace records to skip before writing.")
option(VITASX2_CUBE_EE_TRACE_FROM_BOOT "Start the cube EE trace at BIOS boot instead of ELF entry." OFF)

set(VITASX2_SIMPLE2000_EE_TRACE_RECORDS "4096" CACHE STRING "Simple 2000 EE trace records to write.")
set(VITASX2_SIMPLE2000_EE_TRACE_SKIP_RECORDS "0" CACHE STRING "Simple 2000 EE trace records to skip before writing.")
option(VITASX2_SIMPLE2000_EE_TRACE_FROM_BOOT "Start the Simple 2000 EE trace at BIOS boot instead of ELF entry." OFF)

# --- toolchain/runtime probe -------------------------------------------------
add_executable(vitasx2-probe
	probe/ProbeMain.cpp
)
target_link_libraries(vitasx2-probe
	common
)

vita_create_self(vitasx2-probe.self vitasx2-probe UNSAFE)
vita_create_vpk(vitasx2-probe.vpk ${VITASX2_TITLEID} vitasx2-probe.self
	VERSION "00.01"
	NAME "VitaSX2 Probe"
)

# --- A32 generated-code smoke ----------------------------------------------
add_executable(vitasx2-a32-emitter-smoke
	core/A32EmitterSmokeMain.cpp
)
target_link_libraries(vitasx2-a32-emitter-smoke
	pcsx2core
)

vita_create_self(vitasx2-a32-emitter-smoke.self vitasx2-a32-emitter-smoke UNSAFE)
vita_create_vpk(vitasx2-a32-emitter-smoke.vpk ${VITASX2_A32_EMITTER_SMOKE_TITLEID} vitasx2-a32-emitter-smoke.self
	VERSION "00.01"
	NAME "VitaSX2 A32 Code"
)

# --- A32 EE provider smoke --------------------------------------------------
add_executable(vitasx2-a32-provider-smoke
	core/A32ProviderSmokeMain.cpp
)
target_link_libraries(vitasx2-a32-provider-smoke
	pcsx2core
)

vita_create_self(vitasx2-a32-provider-smoke.self vitasx2-a32-provider-smoke UNSAFE)
vita_create_vpk(vitasx2-a32-provider-smoke.vpk ${VITASX2_A32_PROVIDER_SMOKE_TITLEID} vitasx2-a32-provider-smoke.self
	VERSION "00.01"
	NAME "VitaSX2 A32 EE"
)

# --- interpreter-core memory/vtlb smoke ------------------------------------
add_executable(vitasx2-core-smoke
	core/CoreSmokeMain.cpp
)
target_link_libraries(vitasx2-core-smoke
	pcsx2core
)

vita_create_self(vitasx2-core-smoke.self vitasx2-core-smoke UNSAFE)
vita_create_vpk(vitasx2-core-smoke.vpk ${VITASX2_CORE_SMOKE_TITLEID} vitasx2-core-smoke.self
	VERSION "00.01"
	NAME "VitaSX2 Core Smoke"
)

# --- BIOS/reset smoke -------------------------------------------------------
add_executable(vitasx2-bios-smoke
	core/BiosSmokeMain.cpp
)
target_link_libraries(vitasx2-bios-smoke
	pcsx2core
)

vita_create_self(vitasx2-bios-smoke.self vitasx2-bios-smoke UNSAFE)
vita_create_vpk(vitasx2-bios-smoke.vpk ${VITASX2_BIOS_SMOKE_TITLEID} vitasx2-bios-smoke.self
	VERSION "00.01"
	NAME "VitaSX2 BIOS Smoke"
)

# --- BIOS EE trace smoke ----------------------------------------------------
add_executable(vitasx2-bios-ee-trace
	core/BiosEeTraceMain.cpp
)
target_link_libraries(vitasx2-bios-ee-trace
	pcsx2core
)

vita_create_self(vitasx2-bios-ee-trace.self vitasx2-bios-ee-trace UNSAFE)
vita_create_vpk(vitasx2-bios-ee-trace.vpk ${VITASX2_BIOS_EE_TRACE_TITLEID} vitasx2-bios-ee-trace.self
	VERSION "00.01"
	NAME "VitaSX2 BIOS EE Trace"
)

# --- BIOS IOP trace smoke ---------------------------------------------------
add_executable(vitasx2-bios-iop-trace
	core/BiosIopTraceMain.cpp
)
target_link_libraries(vitasx2-bios-iop-trace
	pcsx2core
)

vita_create_self(vitasx2-bios-iop-trace.self vitasx2-bios-iop-trace UNSAFE)
vita_create_vpk(vitasx2-bios-iop-trace.vpk ${VITASX2_BIOS_IOP_TRACE_TITLEID} vitasx2-bios-iop-trace.self
	VERSION "00.01"
	NAME "VitaSX2 BIOS IOP Trace"
)

# --- Homebrew ELF EE trace smoke -------------------------------------------
add_executable(vitasx2-cube-ee-trace
	core/ElfEeTraceMain.cpp
)
target_compile_definitions(vitasx2-cube-ee-trace PRIVATE
	VITASX2_EE_TRACE_RECORDS=${VITASX2_CUBE_EE_TRACE_RECORDS}
	VITASX2_EE_TRACE_SKIP_RECORDS=${VITASX2_CUBE_EE_TRACE_SKIP_RECORDS}
	VITASX2_EE_TRACE_FROM_BOOT=$<BOOL:${VITASX2_CUBE_EE_TRACE_FROM_BOOT}>
)
target_link_libraries(vitasx2-cube-ee-trace
	pcsx2core
)

vita_create_self(vitasx2-cube-ee-trace.self vitasx2-cube-ee-trace UNSAFE)
vita_create_vpk(vitasx2-cube-ee-trace.vpk ${VITASX2_CUBE_EE_TRACE_TITLEID} vitasx2-cube-ee-trace.self
	VERSION "00.01"
	NAME "VitaSX2 Cube EE Trace"
)

# --- Real-game disc EE trace smoke -----------------------------------------
add_executable(vitasx2-simple2000-ee-trace
	core/DiscEeTraceMain.cpp
)
target_compile_definitions(vitasx2-simple2000-ee-trace PRIVATE
	VITASX2_EE_TRACE_RECORDS=${VITASX2_SIMPLE2000_EE_TRACE_RECORDS}
	VITASX2_EE_TRACE_SKIP_RECORDS=${VITASX2_SIMPLE2000_EE_TRACE_SKIP_RECORDS}
	VITASX2_EE_TRACE_FROM_BOOT=$<BOOL:${VITASX2_SIMPLE2000_EE_TRACE_FROM_BOOT}>
)
target_link_libraries(vitasx2-simple2000-ee-trace
	pcsx2core
)

vita_create_self(vitasx2-simple2000-ee-trace.self vitasx2-simple2000-ee-trace UNSAFE)
vita_create_vpk(vitasx2-simple2000-ee-trace.vpk ${VITASX2_SIMPLE2000_EE_TRACE_TITLEID} vitasx2-simple2000-ee-trace.self
	VERSION "00.01"
	NAME "VitaSX2 Simple2000 EE Trace"
)
