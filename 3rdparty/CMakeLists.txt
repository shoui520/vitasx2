# Dependency subset for the Vita target. Everything here must build with
# arm-vita-eabi; desktop-only dependencies were removed at import time.

# fmt - formatting, used by Console/StringUtil/Error.
add_subdirectory(fmt EXCLUDE_FROM_ALL)

# fast_float - string->float, used by StringUtil.
add_subdirectory(fast_float EXCLUDE_FROM_ALL)

# simpleini - INI settings backend.
add_subdirectory(simpleini EXCLUDE_FROM_ALL)

# Compression/container dependencies used by the PCSX2 CDVD and state code.
find_package(ZLIB REQUIRED)
if(NOT TARGET ZLIB::ZLIB)
	add_library(ZLIB::ZLIB UNKNOWN IMPORTED GLOBAL)
	set_target_properties(ZLIB::ZLIB PROPERTIES
		IMPORTED_LOCATION "${ZLIB_LIBRARIES}"
		INTERFACE_INCLUDE_DIRECTORIES "${ZLIB_INCLUDE_DIRS}"
	)
endif()

if(NOT TARGET Zstd::Zstd)
	find_path(Zstd_INCLUDE_DIR NAMES zstd.h REQUIRED)
	find_library(Zstd_LIBRARY NAMES zstd zstd_static REQUIRED)
	add_library(Zstd::Zstd UNKNOWN IMPORTED GLOBAL)
	set_target_properties(Zstd::Zstd PROPERTIES
		IMPORTED_LOCATION "${Zstd_LIBRARY}"
		INTERFACE_INCLUDE_DIRECTORIES "${Zstd_INCLUDE_DIR}"
	)
endif()

add_subdirectory(lzma EXCLUDE_FROM_ALL)
add_subdirectory(libchdr EXCLUDE_FROM_ALL)
add_subdirectory(libzip EXCLUDE_FROM_ALL)
add_subdirectory(ccc EXCLUDE_FROM_ALL)
add_subdirectory(demangler EXCLUDE_FROM_ALL)

# xxhash - header-only usage via 3rdparty/include (GS hashing).
add_library(xxhash INTERFACE)
target_include_directories(xxhash INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

# rapidjson - header-only.
add_library(rapidjson INTERFACE)
target_include_directories(rapidjson INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/rapidjson/include")
