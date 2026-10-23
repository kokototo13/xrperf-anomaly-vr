cmake_minimum_required(VERSION 3.20)
project(xrperf LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(OPENXR_INCLUDE ${CMAKE_SOURCE_DIR}/external/OpenXR-SDK/include)
set(FSR_INCLUDE ${CMAKE_SOURCE_DIR}/external/FidelityFX-FSR/ffx-fsr)
set(NIS_INCLUDE ${CMAKE_SOURCE_DIR}/external/NVIDIAImageScaling/NIS)

# Shaders are compiled at build time into headers with the bytecode as a byte array
find_program(FXC fxc PATHS "$ENV{WindowsSdkVerBinPath}/x64" REQUIRED)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${SHADER_OUT})
function(add_shader name profile var)
    add_custom_command(
        OUTPUT ${SHADER_OUT}/${name}.h
        COMMAND ${FXC} /nologo /T ${profile} /E main /O3 /I ${FSR_INCLUDE} /I ${NIS_INCLUDE}
                /Fh ${SHADER_OUT}/${name}.h /Vn ${var} ${CMAKE_SOURCE_DIR}/shaders/${name}.hlsl
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/${name}.hlsl ${FSR_INCLUDE}/ffx_a.h ${FSR_INCLUDE}/ffx_fsr1.h
                ${NIS_INCLUDE}/NIS_Main.hlsl ${NIS_INCLUDE}/NIS_Scaler.h
        VERBATIM)
    list(APPEND SHADER_HEADERS ${SHADER_OUT}/${name}.h)
    set(SHADER_HEADERS ${SHADER_HEADERS} PARENT_SCOPE)
endfunction()
add_shader(fullscreen_vs vs_5_0 g_FullscreenVS)
add_shader(fsr_easu_ps ps_5_0 g_FsrEasuPS)
add_shader(fsr_rcas_ps ps_5_0 g_FsrRcasPS)
add_shader(nis_cs cs_5_0 g_NisCS)
add_shader(copy_ps ps_5_0 g_CopyPS)

# OpenXR API layer, loaded by openxr_loader.dll through the implicit layer manifest
add_library(xrperf_layer SHARED
    src/layer.cpp
    src/config.cpp
    src/log.cpp
    src/upscaler.cpp
    src/gpu_timer.cpp
    src/vrs.cpp
    ${SHADER_HEADERS}
)
set(NVAPI_DIR ${CMAKE_SOURCE_DIR}/external/nvapi)
target_include_directories(xrperf_layer PRIVATE ${OPENXR_INCLUDE} ${FSR_INCLUDE} ${NIS_INCLUDE} ${SHADER_OUT} ${NVAPI_DIR})
target_compile_definitions(xrperf_layer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(xrperf_layer PRIVATE d3d11 dxgi ${NVAPI_DIR}/amd64/nvapi64.lib)

# Small console app that creates an OpenXR instance through the game's loader,
# so the layer can be checked without launching the game
add_executable(xrperf_test tools/xrperf_test.cpp)
target_include_directories(xrperf_test PRIVATE ${OPENXR_INCLUDE})
target_compile_definitions(xrperf_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# Everything the layer needs at runtime ends up in one folder
set(XRPERF_OUT ${CMAKE_BINARY_DIR}/out)
set_target_properties(xrperf_layer xrperf_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${XRPERF_OUT}
    LIBRARY_OUTPUT_DIRECTORY ${XRPERF_OUT})

# Optional dev tool: records SteamVR frame timings during a test run.
# Needs the OpenVR SDK in external/openvr (tools/fetch_openvr.ps1).
set(OPENVR_DIR ${CMAKE_SOURCE_DIR}/external/openvr)
if(EXISTS ${OPENVR_DIR}/headers/openvr.h)
    add_executable(vr_timing_logger tools/vr_timing_logger.cpp)
    target_include_directories(vr_timing_logger PRIVATE ${OPENVR_DIR}/headers)
    target_link_libraries(vr_timing_logger PRIVATE ${OPENVR_DIR}/lib/openvr_api.lib)
    target_compile_definitions(vr_timing_logger PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    set_target_properties(vr_timing_logger PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${XRPERF_OUT})
    configure_file(${OPENVR_DIR}/lib/openvr_api.dll ${XRPERF_OUT}/openvr_api.dll COPYONLY)
endif()
configure_file(manifest/xrperf_layer.json ${XRPERF_OUT}/xrperf_layer.json COPYONLY)
# The ini is the user's live config (the .bat switches edit it): only seed it, never overwrite
if(NOT EXISTS ${XRPERF_OUT}/xrperf.ini)
    file(COPY_FILE ${CMAKE_SOURCE_DIR}/manifest/xrperf.ini ${XRPERF_OUT}/xrperf.ini)
endif()
