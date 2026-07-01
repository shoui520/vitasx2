# PCSX2 common/ support library, cut down to what the Vita can host.
#
# Excluded from upstream and why:
#   HTTPDownloader*        - no network features on the Vita target
#   Image.cpp              - needs libjpeg/libpng/libwebp; screenshots come later
#   YAML.cpp               - needs rapidyaml; GameDatabase is not in the first build
#   DynamicLibrary.cpp     - no dlopen on Vita
#   StackWalker.cpp        - Windows-only
#   CocoaTools.mm, Darwin/, Linux/, Windows/ - desktop host layers, replaced by Vita/
#   emitter/               - x86-64 emitter; the A32 emitter replaces it under vita/
#   FastJmp.asm            - x86 MASM; A32 implementation lives in Vita/VitaFastJmp.S

add_library(common STATIC
	AlignedMalloc.cpp
	Assertions.cpp
	Console.cpp
	CrashHandler.cpp
	Error.cpp
	FastJmp.cpp
	FileSystem.cpp
	MD5Digest.cpp
	MemoryInterface.cpp
	MemorySettingsInterface.cpp
	Perf.cpp
	ProgressCallback.cpp
	ReadbackSpinManager.cpp
	Semaphore.cpp
	SettingsWrapper.cpp
	SmallString.cpp
	StringUtil.cpp
	TextureDecompress.cpp
	Timer.cpp
	WAVWriter.cpp
	WindowInfo.cpp
	Vita/VitaHostSys.cpp
	Vita/VitaMisc.cpp
	Vita/VitaThreads.cpp
)

target_include_directories(common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(common PUBLIC
	fmt::fmt
	fast_float
	pthread
	SceLibKernel_stub
	SceKernelThreadMgr_stub
	SceProcessmgr_stub
	SceIofilemgr_stub
	SceSysmem_stub
	ScePower_stub
	SceAppMgr_stub
)
