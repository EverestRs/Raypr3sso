cmake_minimum_required(VERSION 3.18)
project(Raypr3sso
    VERSION 0.1.0
    DESCRIPTION "CUDA Path Tracing Renderer"
    LANGUAGES CXX CUDA
)

find_package(Threads REQUIRED)
find_package(CUDAToolkit REQUIRED)

# ---- C++ Standard ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

# ---- Options ----
option(RENDERER_USE_TINYEXR "Enable OpenEXR output via tinyexr" OFF)
# Perf option: --use_fast_math on the CUDA kernels (~5-10% measured on this
# renderer family). It trades IEEE strictness for speed and therefore BREAKS
# the byte-level A/B regression standard (ulp-level FP differences everywhere);
# keep OFF for verification builds, enable for throughput experiments.
option(RENDERER_FAST_MATH "Enable --use_fast_math for CUDA kernels (breaks byte-level reproducibility)" OFF)
# Warnings: on by default but never fatal — the codebase predates them and CI
# only needs to surface new ones. Flip RENDERER_WERROR on for a clean branch.
option(RENDERER_WARNINGS "Enable -Wall -Wextra for host/CUDA host code" ON)
option(RENDERER_WERROR "Treat warnings as errors" OFF)

function(raypr3sso_enable_warnings target)
    if(RENDERER_WARNINGS)
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-Wall>
            $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
            $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-Wall,-Wextra>)
    endif()
    if(RENDERER_WERROR)
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-Werror>
            $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-Werror>)
    endif()
endfunction()

# ---- CUDA Architecture ----
# Target GPU: NVIDIA Tesla V100 (sm_70, Volta).
# CUDA 13.0 dropped sm_70 support, so we use CUDA 12.6 for native sm_70.
# Override with: -DCUDA_ARCH=86 (for RTX 3090), -DCUDA_ARCH=90 (for H100), etc.
set(CUDA_ARCH "70" CACHE STRING "CUDA compute capability (e.g. 70, 75, 86)")
set(CMAKE_CUDA_ARCHITECTURES ${CUDA_ARCH})

# Prefer CUDA 12.6 (supports sm_70 for V100) over CUDA 13.0 (dropped sm_70).
# The CUDA 13.0 driver still runs the binary correctly on V100.
if(EXISTS "/usr/local/cuda-12.6/bin/nvcc")
    set(CMAKE_CUDA_COMPILER "/usr/local/cuda-12.6/bin/nvcc" CACHE STRING "CUDA compiler (12.6 for V100 sm_70 support)")
    message(STATUS "Using CUDA 12.6 from /usr/local/cuda-12.6 (for V100 sm_70 support)")
elseif(EXISTS "/usr/local/cuda-12/bin/nvcc")
    set(CMAKE_CUDA_COMPILER "/usr/local/cuda-12/bin/nvcc" CACHE STRING "CUDA compiler")
    message(STATUS "Using CUDA 12 from /usr/local/cuda-12")
endif()

add_definitions(-DRENDERER_HAS_CUDA)

# ---- External Dependencies ----
set(EXTERNAL_DIR "${CMAKE_SOURCE_DIR}/external")

# glm (header-only math library)
if(EXISTS "${EXTERNAL_DIR}/glm/glm/glm.hpp")
    message(STATUS "Using local glm from ${EXTERNAL_DIR}/glm")
    add_library(glm INTERFACE)
    target_include_directories(glm SYSTEM INTERFACE "${EXTERNAL_DIR}/glm")
else()
    message(STATUS "glm not found locally, fetching via FetchContent")
    include(FetchContent)
    FetchContent_Declare(
        glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG 1.0.1
    )
    FetchContent_MakeAvailable(glm)
endif()

# stb_image (header-only)
if(EXISTS "${EXTERNAL_DIR}/stb/stb_image.h")
    message(STATUS "Using local stb from ${EXTERNAL_DIR}/stb")
    add_library(stb INTERFACE)
    target_include_directories(stb SYSTEM INTERFACE "${EXTERNAL_DIR}/stb")
else()
    message(STATUS "stb not found locally, fetching via FetchContent")
    include(FetchContent)
    FetchContent_Declare(
        stb
        GIT_REPOSITORY https://github.com/nothings/stb.git
        GIT_TAG master
    )
    FetchContent_MakeAvailable(stb)
    add_library(stb INTERFACE)
    target_include_directories(stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")
endif()

# assimp (optional, for FBX/glTF model loading)
find_package(assimp QUIET)
if(assimp_FOUND)
    message(STATUS "Found assimp — FBX loading enabled")
    add_definitions(-DRENDERER_HAS_ASSIMP)
else()
    message(STATUS "assimp not found — FBX loading disabled (install libassimp-dev for support)")
endif()

# OIDN (optional, P4c): prebuilt Intel Open Image Denoise under
# external/oidn/oidn-*/ — enables RENDER_OIDN=1 (learned denoiser with
# albedo/normal guides).
file(GLOB OIDN_DIRS "${EXTERNAL_DIR}/oidn/oidn-*.x86_64.linux")
if(OIDN_DIRS)
    list(GET OIDN_DIRS 0 OIDN_DIR)
    message(STATUS "Found OIDN at ${OIDN_DIR} — RENDER_OIDN enabled")
    add_definitions(-DRENDERER_HAS_OIDN)
    set(OIDN_INCLUDE_DIR "${OIDN_DIR}/include")
    set(OIDN_LIB "${OIDN_DIR}/lib/libOpenImageDenoise.so")
endif()

# tinyexr (optional)
if(RENDERER_USE_TINYEXR)
    if(EXISTS "${EXTERNAL_DIR}/tinyexr/tinyexr.h")
        message(STATUS "Using local tinyexr from ${EXTERNAL_DIR}/tinyexr")
        add_library(tinyexr INTERFACE)
        target_include_directories(tinyexr SYSTEM INTERFACE "${EXTERNAL_DIR}/tinyexr")
    else()
        message(STATUS "tinyexr not found locally")
    endif()
endif()

# ---- Core library (shared by the CLI and the render server) ----
# Both executables used to list the same engine sources; adding a file meant
# editing two lists. The core static library owns the engine, the executables
# are thin entry points.
set(CORE_SOURCES
    src/geometry.cpp
    src/component.cpp
    src/material.cpp
    src/model.cpp
    src/bvh.cpp
    src/sampling.cpp
    src/render.cpp
    src/camera.cpp
    src/image.cpp
    src/console.cpp
    src/utils.cpp
    src/gpu_utils.cpp
    src/gpu_scene.cpp
    src/render_job.cpp
    src/http_util.cpp
    src/render_server.cpp
)

set(CORE_CUDA_SOURCES
    # One explicit CUDA TU. cuda_tu.cu implementation-includes bvh_gpu.cu,
    # render_gpu.cu and render_wavefront.cu (separable compilation is off —
    # see the linker note below). This is the ONLY file that may #include a
    # .cu implementation.
    src/cuda_tu.cu
    # GPU post-processing pipeline (RENDER_POST_GPU=1) — mirrors image.cpp
    src/image_gpu.cu
)

add_library(render_core STATIC ${CORE_SOURCES} ${CORE_CUDA_SOURCES})

target_include_directories(render_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_definitions(render_core PUBLIC RENDERER_HAS_CUDA)
target_link_libraries(render_core PUBLIC
    glm
    stb
    Threads::Threads
    CUDA::cudart
)
if(assimp_FOUND)
    target_link_libraries(render_core PUBLIC assimp::assimp)
endif()
if(OIDN_DIRS)
    target_include_directories(render_core SYSTEM PUBLIC "${OIDN_INCLUDE_DIR}")
    target_link_libraries(render_core PUBLIC "${OIDN_LIB}")
endif()
if(RENDERER_USE_TINYEXR)
    target_link_libraries(render_core PUBLIC tinyexr)
    target_compile_definitions(render_core PUBLIC RENDERER_HAS_TINYEXR)
endif()

# Disable separable compilation: the device linker strips PTX, but V100
# needs PTX (sm_70 is not supported by CUDA 13.0's SASS compiler).
# Consequence: every cross-file device call lives in ONE translation unit,
# src/cuda_tu.cu.
# set_property(TARGET render_core PROPERTY CUDA_SEPARABLE_COMPILATION ON)

# ---- Executables (thin entry points) ----
add_executable(Raypr3sso src/main.cpp)
target_link_libraries(Raypr3sso PRIVATE render_core)

add_executable(render_server src/render_server_main.cpp)
target_link_libraries(render_server PRIVATE render_core)

if(OIDN_DIRS)
    set_target_properties(Raypr3sso PROPERTIES
        BUILD_RPATH "${OIDN_DIR}/lib"
        INSTALL_RPATH "${OIDN_DIR}/lib")
    set_target_properties(render_server PROPERTIES BUILD_RPATH "${OIDN_DIR}/lib")
endif()

# ---- CUDA specific flags ----
target_compile_options(render_core PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:--extra-device-vectorization>
    $<$<COMPILE_LANGUAGE:CUDA>:-lineinfo>
)
if(RENDERER_FAST_MATH)
    target_compile_options(render_core PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:--use_fast_math>)
endif()

# ---- Platform specific ----
if(WIN32)
    target_compile_definitions(render_core PUBLIC _USE_MATH_DEFINES)
endif()

raypr3sso_enable_warnings(render_core)
raypr3sso_enable_warnings(Raypr3sso)
raypr3sso_enable_warnings(render_server)

# ---- Install ----
install(TARGETS Raypr3sso DESTINATION bin)

# ---- Print config summary ----
message(STATUS "Raypr3sso Configuration:")
message(STATUS "  CUDA Architecture : ${CUDA_ARCH}")
message(STATUS "  tinyexr support   : ${RENDERER_USE_TINYEXR}")
message(STATUS "  fast_math         : ${RENDERER_FAST_MATH}")
message(STATUS "  warnings          : ${RENDERER_WARNINGS} (Werror: ${RENDERER_WERROR})")
message(STATUS "  Build type        : ${CMAKE_BUILD_TYPE}")
