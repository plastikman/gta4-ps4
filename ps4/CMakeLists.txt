# GTA IV for PS4: Graine/ReXGlue, linked with the Orbis RADV bundle.
# Configure with the bundle toolchain documented in ps4/BUILD.md.
cmake_minimum_required(VERSION 3.25)
project(gta4-ps4 LANGUAGES C CXX)

if(NOT PS4)
    message(FATAL_ERROR "Use the Orbis bundle's toolchain/orbis-sdk.cmake toolchain file")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS OFF)

# Complete C++20/23 facilities missing from the SDK bundled libc++ headers.
if(NOT CMAKE_C_FLAGS MATCHES "-Doff64_t=off_t")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mssse3 -Doff64_t=off_t -Dfseeko64=fseeko -Dftello64=ftello -Dftruncate64=ftruncate" CACHE STRING "" FORCE)
endif()
if(NOT CMAKE_CXX_FLAGS MATCHES "orbis_cxx_compat.h")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -I${CMAKE_CURRENT_LIST_DIR}/include -mssse3 -Doff64_t=off_t -Dfseeko64=fseeko -Dftello64=ftello -Dftruncate64=ftruncate -include ${CMAKE_CURRENT_LIST_DIR}/include/orbis_cxx_compat.h" CACHE STRING "" FORCE)
endif()
# The PS4's CPU (AMD Jaguar, btver2), as Sony's toolchain targets by default:
# MOVBE (a byte-swapped load or store in one instruction: every guest memory
# access), LZCNT/POPCNT/BMI1, SSE4.2. No FMA, so REX_FMA stays a*b+c.
# AVX off: the SDK's intrinsic headers lack the 256-bit types (snappy uses them
# when __AVX__ is set), and Jaguar splits 256-bit work in two anyway.
add_compile_options(
    $<$<NOT:$<COMPILE_LANGUAGE:ASM_NASM>>:-march=btver2>
    $<$<NOT:$<COMPILE_LANGUAGE:ASM_NASM>>:-mno-avx>
)
set(LIBERTY_RECOMP_TARGET_PLATFORM "ps4" CACHE STRING "" FORCE)
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../glue/rexglue-sdk-main"
    CACHE PATH "Path to the Graine ReXGlue SDK")
set(REXGLUE_USE_VULKAN ON CACHE BOOL "Use Orbis RADV" FORCE)
set(REXGLUE_USE_D3D12 OFF CACHE BOOL "" FORCE)
set(REXGLUE_ENABLE_FIDELITYFX OFF CACHE BOOL "Optional FidelityFX providers" FORCE)
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "Tracy is not enabled in the console target" FORCE)
set(REXGLUE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
link_directories("${ORBIS_MESA_BUILD}/subprojects/zlib-1.3.1")
if(NOT PS4_ZLIB_INCLUDE_DIR OR NOT EXISTS "${PS4_ZLIB_INCLUDE_DIR}/zlib.h")
    message(FATAL_ERROR
        "PS4_ZLIB_INCLUDE_DIR must point to the pinned zlib headers. "
        "Run ps4/build-clean.sh to fetch and verify them.")
endif()

if(NOT ORBIS_COMPAT_DIR OR NOT ORBIS_MESA_SRC OR NOT ORBIS_MESA_BUILD)
    message(FATAL_ERROR
        "The Orbis SDK toolchain must define ORBIS_COMPAT_DIR, ORBIS_MESA_SRC, "
        "and ORBIS_MESA_BUILD. Use the bundled toolchain/orbis-sdk.cmake.")
endif()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../glue" glue)

