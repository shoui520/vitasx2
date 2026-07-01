# Vita-native layer: host runtime, probes, GXM backend, A32 emitters.

set(VITASX2_TITLEID "VSX200001")
set(VITASX2_CORE_SMOKE_TITLEID "VSX200002")

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
