set(REXGLUE_USE_VULKAN ON CACHE BOOL "" FORCE)
set(REXGLUE_USE_D3D12 OFF CACHE BOOL "" FORCE)
set(REXGLUE_ENABLE_FIDELITYFX OFF CACHE BOOL "Enable optional FidelityFX temporal providers")
set(REXGLUE_BUILD_TESTS OFF CACHE BOOL "")

set(REXSDK_DIR "${CMAKE_SOURCE_DIR}/glue/rexglue-sdk-main"
    CACHE PATH "Path to the Graine RexGlue SDK source tree")

if(NOT EXISTS "${REXSDK_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Graine RexGlue SDK not found at ${REXSDK_DIR}")
endif()

add_subdirectory("${REXSDK_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/rexglue-sdk")

# The Orbis bundle pairs its compatibility layer and RADV build. Its
# vkloader supplies the Vulkan C ABI that a PS4 process cannot load at runtime.
if(PS4)
    if(NOT ORBIS_COMPAT_DIR OR NOT EXISTS "${ORBIS_COMPAT_DIR}/vkloader/CMakeLists.txt")
        message(FATAL_ERROR
            "PS4 Vulkan support requires ORBIS_COMPAT_DIR from the Orbis SDK bundle; "
            "no vkloader/CMakeLists.txt was found there.")
    endif()
    add_subdirectory("${ORBIS_COMPAT_DIR}/vkloader" ps4-vkloader)
endif()

# The macOS consumer must package the workspace MoltenVK build.  The generic
# RexGlue staging code still supplies the Vulkan loader and SDK support files,
# but an installed MoltenVK may not contain the fixes carried by this tree.
if(APPLE AND NOT TARGET MoltenVK)
    add_subdirectory(
        "${CMAKE_SOURCE_DIR}/thirdparty/MoltenVK"
        "${CMAKE_BINARY_DIR}/thirdparty/MoltenVK"
    )
endif()

add_subdirectory("${REXSDK_DIR}/gta4-recomp" "${CMAKE_CURRENT_BINARY_DIR}/gta4-recomp")
