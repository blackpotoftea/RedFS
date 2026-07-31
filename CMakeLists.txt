cmake_minimum_required(VERSION 3.21)
project(RedFS VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(REDFS_BUILD_SHARED "Build RedFS.dll alongside the static library" ON)
option(REDFS_BUILD_CLI    "Build the redfs_cli verification harness"     ON)
option(REDFS_BUILD_TESTS  "Build unit tests and the parser fuzzer"       ON)
option(REDFS_ANALYZE      "Enable MSVC /analyze static analysis"         OFF)

# address | undefined | none. ASan catches out-of-bounds and use-after-free in
# the parsers; the tests and fuzzer become memory-safety sweeps under it.
set(REDFS_SANITIZE "none" CACHE STRING "Sanitizer: none | address")

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)

    if(REDFS_ANALYZE)
        # MSVC's own static analyser. Noisy on system headers, so scope it to our
        # code and keep it off by default.
        add_compile_options(/analyze /analyze:external- /external:anglebrackets /external:W0)
    endif()

    if(REDFS_SANITIZE STREQUAL "address")
        add_compile_options(/fsanitize=address /Zi)
        add_compile_definitions(REDFS_ASAN)
        # ASan is incompatible with the incremental linker and with /RTC.
        add_link_options(/INCREMENTAL:NO /DEBUG)
        string(REGEX REPLACE "/RTC[1csu]*" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        message(STATUS "RedFS: AddressSanitizer enabled")
    endif()
endif()

set(REDFS_SOURCES
    src/api.cpp
    src/archive.cpp
    src/cache.cpp
    src/cr2w.cpp
    src/formats.cpp
    src/mesh.cpp
    src/oodle.cpp
    src/paths.cpp
)

# --- static library: link this into a mod for a single self-contained DLL ------
add_library(redfs_static STATIC ${REDFS_SOURCES})
target_include_directories(redfs_static PUBLIC include PRIVATE src)
target_compile_definitions(redfs_static PUBLIC REDFS_STATIC)
add_library(RedFS::static ALIAS redfs_static)

# --- shared library: one copy shared by several mods --------------------------
if(REDFS_BUILD_SHARED)
    add_library(redfs_shared SHARED ${REDFS_SOURCES})
    target_include_directories(redfs_shared PUBLIC include PRIVATE src)
    target_compile_definitions(redfs_shared PRIVATE REDFS_BUILD_DLL)
    set_target_properties(redfs_shared PROPERTIES OUTPUT_NAME RedFS)
    add_library(RedFS::shared ALIAS redfs_shared)
endif()

if(REDFS_BUILD_CLI)
    add_executable(redfs_cli tools/redfs_cli.cpp)
    # bcrypt for `verify`: the SHA-1 it checks against the archive index has to
    # come from outside RedFS to be worth anything, so it uses the system one.
    target_link_libraries(redfs_cli PRIVATE redfs_static bcrypt)

    # Cross-checks our DDS output against DirectXTex. Needs a texconv.dll at
    # runtime (WolvenKit ships one under WolvenKit.Common/lib).
    add_executable(redfs_verify tools/redfs_verify.cpp)
    target_link_libraries(redfs_verify PRIVATE redfs_static)

    # Worked example; also compile-checks the header-only C++ wrapper.
    add_executable(example_chest tools/example_chest.cpp)
    target_link_libraries(example_chest PRIVATE redfs_static)
endif()

# --- RED4ext plugin (optional) -------------------------------------------------
#
# The only thing in this repository that runs inside the game. Off unless you
# point REDFS_RED4EXT_SDK at a checkout of https://github.com/WopsS/RED4ext.SDK,
# because most people building RedFS do not have one and a missing dependency
# should not break the build for them.
#
#   cmake -B build -DREDFS_RED4EXT_SDK=C:/path/to/RED4ext.SDK
#
# Statically linked on purpose: a plugin should ship as one self-contained DLL,
# and it removes any question of which module owns the depot.
set(REDFS_RED4EXT_SDK "" CACHE PATH "Path to a RED4ext.SDK checkout; enables the in-game plugin")

if(REDFS_RED4EXT_SDK)
    if(NOT EXISTS "${REDFS_RED4EXT_SDK}/include/RED4ext/RED4ext.hpp")
        message(FATAL_ERROR
            "REDFS_RED4EXT_SDK=${REDFS_RED4EXT_SDK} does not look like a RED4ext.SDK checkout "
            "(no include/RED4ext/RED4ext.hpp)")
    endif()
    add_library(redfs_red4ext_plugin SHARED examples/red4ext_plugin/Main.cpp)
    target_link_libraries(redfs_red4ext_plugin PRIVATE redfs_static)
    target_include_directories(redfs_red4ext_plugin PRIVATE "${REDFS_RED4EXT_SDK}/include")
    # The SDK needs a recent standard and leans on Windows headers being unpolluted.
    target_compile_features(redfs_red4ext_plugin PRIVATE cxx_std_20)
    set_target_properties(redfs_red4ext_plugin PROPERTIES OUTPUT_NAME RedFS.SmokeTest)
    message(STATUS "RED4ext plugin enabled (SDK: ${REDFS_RED4EXT_SDK})")
endif()

# --- tests --------------------------------------------------------------------
# These need no game install: every input is synthesized by tests/fixtures.cpp.
if(REDFS_BUILD_TESTS)
    enable_testing()

    add_executable(redfs_test
        tests/test_main.cpp
        tests/test_redfs.cpp
        tests/fixtures.cpp
    )
    target_link_libraries(redfs_test PRIVATE redfs_static)
    target_include_directories(redfs_test PRIVATE tests)

    add_executable(redfs_fuzz
        tests/fuzz_redfs.cpp
        tests/fixtures.cpp
    )
    target_link_libraries(redfs_fuzz PRIVATE redfs_static)
    target_include_directories(redfs_fuzz PRIVATE tests)

    # Teardown behaviour: abrupt exit, shutdown under load, and a real
    # LoadLibrary/FreeLibrary cycle. Spawns child processes, so it needs its own
    # binary rather than being a case in redfs_test.
    add_executable(redfs_lifecycle
        tests/lifecycle_test.cpp
        tests/fixtures.cpp
    )
    target_link_libraries(redfs_lifecycle PRIVATE redfs_static)
    target_include_directories(redfs_lifecycle PRIVATE tests)
    if(REDFS_BUILD_SHARED)
        # The DLL unload scenario loads this at runtime from the output directory.
        add_dependencies(redfs_lifecycle redfs_shared)
    endif()

    add_test(NAME unit COMMAND redfs_test)
    # Short by default so `ctest` stays quick; run redfs_fuzz directly for longer.
    add_test(NAME fuzz COMMAND redfs_fuzz 4000 1)
    add_test(NAME lifecycle COMMAND redfs_lifecycle)
endif()
