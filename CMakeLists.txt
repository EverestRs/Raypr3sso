cmake_minimum_required(VERSION 3.18)
project(Raypr3sso
    VERSION 0.1.0
    DESCRIPTION "CUDA Path Tracing Renderer"
    LANGUAGES CXX CUDA
)

find_package(Threads REQUIRED)

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
    target_include_directories(glm INTERFACE "${EXTERNAL_DIR}/glm")
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
    target_include_directories(stb INTERFACE "${EXTERNAL_DIR}/stb")
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
    target_include_directories(stb INTERFACE "${stb_SOURCE_DIR}")
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
        target_include_directories(tinyexr INTERFACE "${EXTERNAL_DIR}/tinyexr")
    else()
        message(STATUS "tinyexr not found locally")
    endif()
endif()

# ---- Source Files ----
set(SOURCES
    src/main.cpp
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
)

set(CUDA_SOURCES
    # bvh_gpu.cu is #included from render_gpu.cu to avoid linker stripping PTX
    src/render_gpu.cu
    # GPU post-processing pipeline (RENDER_POST_GPU=1) — mirrors image.cpp
    src/image_gpu.cu
)

# ---- Main Executable ----
add_executable(Raypr3sso ${SOURCES} ${CUDA_SOURCES})

add_executable(render_server
    src/render_server_main.cpp
    src/render_server.cpp
    src/render_job.cpp
    src/model.cpp
    src/geometry.cpp
    src/component.cpp
    src/material.cpp
    src/bvh.cpp
    src/sampling.cpp
    src/camera.cpp
    src/utils.cpp
    src/gpu_utils.cpp
    src/gpu_scene.cpp
    src/image.cpp
    src/image_gpu.cu
    src/render.cpp
    src/render_gpu.cu
)
target_include_directories(render_server PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_definitions(render_server PRIVATE RENDERER_HAS_CUDA)
target_link_libraries(render_server PRIVATE Threads::Threads glm stb)
if(assimp_FOUND)
    target_link_libraries(render_server PRIVATE assimp::assimp)
endif()
if(OIDN_DIRS)
    target_include_directories(render_server PRIVATE "${OIDN_INCLUDE_DIR}")
    target_link_libraries(render_server PRIVATE "${OIDN_LIB}")
    set_target_properties(render_server PROPERTIES BUILD_RPATH "${OIDN_DIR}/lib")
endif()
# Disable separable compilation: the device linker strips PTX, but V100
# needs PTX (sm_70 is not supported by CUDA 13.0's SASS compiler).
# Instead, bvh_gpu.cu is #included from render_gpu.cu to avoid cross-file
# device function calls.
# set_property(TARGET Raypr3sso PROPERTY CUDA_SEPARABLE_COMPILATION ON)

target_include_directories(Raypr3sso PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(Raypr3sso PRIVATE
    glm
    stb
    Threads::Threads
)
if(assimp_FOUND)
    target_link_libraries(Raypr3sso PRIVATE assimp::assimp)
endif()

if(OIDN_DIRS)
    target_include_directories(Raypr3sso PRIVATE "${OIDN_INCLUDE_DIR}")
    target_link_libraries(Raypr3sso PRIVATE "${OIDN_LIB}")
    set_target_properties(Raypr3sso PROPERTIES
        BUILD_RPATH "${OIDN_DIR}/lib"
        INSTALL_RPATH "${OIDN_DIR}/lib")
endif()

if(RENDERER_USE_TINYEXR)
    target_link_libraries(Raypr3sso PRIVATE tinyexr)
    target_compile_definitions(Raypr3sso PRIVATE RENDERER_HAS_TINYEXR)
endif()

# ---- CUDA specific flags ----
target_compile_options(Raypr3sso PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:--extra-device-vectorization>
    $<$<COMPILE_LANGUAGE:CUDA>:-lineinfo>
)
if(RENDERER_FAST_MATH)
    target_compile_options(Raypr3sso PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:--use_fast_math>)
    target_compile_options(render_server PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:--use_fast_math>)
endif()

# ---- Platform specific ----
if(WIN32)
    target_compile_definitions(Raypr3sso PRIVATE _USE_MATH_DEFINES)
endif()

# ---- Install ----
install(TARGETS Raypr3sso DESTINATION bin)

# ---- Print config summary ----
message(STATUS "Raypr3sso Configuration:")
message(STATUS "  CUDA Architecture : ${CUDA_ARCH}")
message(STATUS "  tinyexr support   : ${RENDERER_USE_TINYEXR}")
message(STATUS "  fast_math         : ${RENDERER_FAST_MATH}")
message(STATUS "  Build type        : ${CMAKE_BUILD_TYPE}")
