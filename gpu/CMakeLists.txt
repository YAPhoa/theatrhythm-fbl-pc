# theatrhythm GPU library: shadPS4 video core + shader recompiler + Gnm/VideoOut
# drivers, adapted to the native_probe runtime through shims in shim/.
cmake_minimum_required(VERSION 3.24)
project(theatrhythm_gpu LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Frame pointers keep the loader's thread dumps (watchdog, SIGUSR2 sampling) walkable.
add_compile_options(-fno-omit-frame-pointer)

find_package(Vulkan REQUIRED)
find_package(fmt REQUIRED)
find_package(Boost REQUIRED)
# theatrhythm (Windows): MSYS2 has no magic_enum/miniz packages; the submodules are used instead.
find_package(magic_enum QUIET)
if (NOT magic_enum_FOUND)
    add_library(magic_enum INTERFACE)
    add_library(magic_enum::magic_enum ALIAS magic_enum)
    target_include_directories(magic_enum INTERFACE third_party/magic_enum/include
                                                    third_party/magic_enum/include/magic_enum)
endif()
find_package(tsl-robin-map REQUIRED)
find_package(VulkanMemoryAllocator CONFIG QUIET)
if (NOT VulkanMemoryAllocator_FOUND)
    # MSYS2 ships the header (include/vma/vk_mem_alloc.h) without a CMake package.
    find_path(VMA_INCLUDE_DIR vk_mem_alloc.h PATH_SUFFIXES vma REQUIRED)
    add_library(VulkanMemoryAllocator INTERFACE)
    add_library(GPUOpen::VulkanMemoryAllocator ALIAS VulkanMemoryAllocator)
    target_include_directories(VulkanMemoryAllocator INTERFACE ${VMA_INCLUDE_DIR})
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
if (NOT WIN32)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
endif()
find_package(miniz QUIET)
if (NOT miniz_FOUND)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    add_subdirectory(third_party/miniz EXCLUDE_FROM_ALL)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
    add_library(miniz::miniz ALIAS miniz)
endif()
find_package(Zydis REQUIRED)
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)

set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/sirit EXCLUDE_FROM_ALL)

# Host shaders -> headers (same scheme as shadPS4's host_shaders/CMakeLists.txt).
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS ${SHADER_SRC}/${FILENAME} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(post_process.frag post_process_frag)
add_host_shader(buffer_multi_copy.comp buffer_multi_copy_comp)
add_host_shader(depth_resample.frag depth_resample_frag)
add_host_shader(depth_resample.frag depth_stencil_resample_frag HAS_STENCIL=1)
add_host_shader(depth_resample.frag depth_stencil_bits_frag STENCIL_BITS=1)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS shadps4/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS shadps4/shader_recompiler/*.cpp)
set(COMMON
    shadps4/common/decoder.cpp shadps4/common/error.cpp
    shadps4/common/io_file.cpp shadps4/common/native_clock.cpp shadps4/common/number_utils.cpp
    shadps4/common/path_util.cpp shadps4/common/rdtsc.cpp shadps4/common/spin_lock.cpp shadps4/common/recursive_lock.cpp
    shadps4/common/string_util.cpp shadps4/common/thread.cpp shadps4/common/signal_context.cpp
    shim/common/logging/log.cpp)
file(GLOB AVPLAYER CONFIGURE_DEPENDS shadps4/core/libraries/avplayer/*.cpp)
# Upstream HLE libraries compiled unchanged (or with "theatrhythm:" patches); the C runtime
# resolves first, so these only answer imports it does not implement.
file(GLOB HLE_LIBRARIES CONFIGURE_DEPENDS
    shadps4/core/libraries/hmd/*.cpp shadps4/core/libraries/vr_tracker/*.cpp
    shadps4/core/libraries/move/*.cpp shadps4/core/libraries/remote_play/*.cpp
    shadps4/core/libraries/web_browser_dialog/*.cpp shadps4/core/libraries/disc_map/*.cpp
    shadps4/core/libraries/network/ssl.cpp
    shadps4/core/libraries/game_live_streaming/*.cpp shadps4/core/libraries/screenshot/*.cpp
    shadps4/core/libraries/share_play/*.cpp shadps4/core/libraries/video_recording/*.cpp)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/*.cpp shim/*/*.cpp)
# Static and linked into theatrhythm-fbl, whose runtime_* symbols it calls (a Windows DLL cannot
# leave them unresolved; Linux uses the same single executable).
add_library(theatrhythm_gpu STATIC ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES}
    shadps4/core/libraries/gnmdriver/gnmdriver.cpp
    shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
    shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER} ${HLE_LIBRARIES}
    shadps4/core/libraries/videodec/video_utils.cpp)
add_dependencies(theatrhythm_gpu host_shaders)
# The PS4 system UI overlay (save list, text entry, trophy notices): Dear ImGui
# (third_party/imgui, MIT) with its Vulkan backend and an embedded font (third_party/fonts).
target_sources(theatrhythm_gpu PRIVATE third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(theatrhythm_gpu PRIVATE third_party/imgui third_party/imgui/backends)
target_compile_definitions(theatrhythm_gpu PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    THEATRHYTHM_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf")
set_source_files_properties(shim/theatrhythm_overlay.cpp PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf")
target_include_directories(theatrhythm_gpu PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/half/include)
target_compile_definitions(theatrhythm_gpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
target_compile_options(theatrhythm_gpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
target_link_libraries(theatrhythm_gpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
    tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::FFMPEG miniz::miniz Zydis::Zydis sirit)
target_include_directories(theatrhythm_gpu PRIVATE third_party/xbyak)
if (WIN32)
    # Guest code is entered with the System V ABI; NOMINMAX/WIN32_LEAN_AND_MEAN keep
    # <windows.h> out of the way of std::min/max and the Vulkan/SDL headers.
    target_compile_definitions(theatrhythm_gpu PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(theatrhythm_gpu PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/shim/theatrhythm_win_compat.h)
    target_link_libraries(theatrhythm_gpu PUBLIC Vulkan::Vulkan ws2_32 bcrypt ntdll synchronization winmm pdh dxgi)
else()
    target_link_libraries(theatrhythm_gpu PUBLIC PkgConfig::X11 Vulkan::Vulkan)
endif()

# theatrhythm: the loader and the HLE runtime, linked with theatrhythm_gpu here, where CMake knows
# its dependencies (build.sh builds them with ninja).
# The Windows counterparts of the POSIX calls (src/compat_win.h) and their libraries.
if (WIN32)
    set(THEATRHYTHM_COMPAT ../src/compat_win.c)
    set(THEATRHYTHM_COMPAT_LIBS bcrypt)
else()
    set(THEATRHYTHM_COMPAT "")
    set(THEATRHYTHM_COMPAT_LIBS "")
    # -std=c11 hides POSIX from glibc's headers (sigjmp_buf in runtime.h, among others).
    add_compile_options($<$<COMPILE_LANGUAGE:C>:-D_DEFAULT_SOURCE>)
endif()
file(GLOB THEATRHYTHM_RUNTIME CONFIGURE_DEPENDS ../src/runtime*.c)
file(GLOB THEATRHYTHM_ATRAC9 CONFIGURE_DEPENDS ../third_party/LibAtrac9/C/src/*.c)
add_library(atrac9 STATIC ${THEATRHYTHM_ATRAC9})
target_compile_options(atrac9 PRIVATE -w)
set_target_properties(atrac9 PROPERTIES C_STANDARD 99 INTERPROCEDURAL_OPTIMIZATION OFF)
# isa_emulate runs inside guest code: no XMM/YMM.
set_source_files_properties(../src/isa_emulate.c PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
add_executable(theatrhythm-fbl ../src/probe.c ${THEATRHYTHM_RUNTIME} ${THEATRHYTHM_COMPAT} ../src/cpu_check.c
    ../src/vulkan_smoke.c ../src/isa_rewrite.c ../src/isa_emulate.c ../src/launch.c)
set_target_properties(theatrhythm-fbl PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(theatrhythm-fbl PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror>)
target_include_directories(theatrhythm-fbl PRIVATE .. ../src)
target_link_libraries(theatrhythm-fbl PRIVATE theatrhythm_gpu atrac9 -pthread)
if (WIN32)
    enable_language(RC)
    target_sources(theatrhythm-fbl PRIVATE ../packaging/windows/theatrhythm-fbl.rc)
    target_link_libraries(theatrhythm-fbl PRIVATE onecore bcrypt ws2_32)
    # The PS4 range (64 GiB - 1 TiB) must stay free: no high-entropy ASLR for heaps and stacks.
    # The guest main thread runs on the process stack: 16 MiB, committed (no stack probes).
    target_link_options(theatrhythm-fbl PRIVATE -Wl,--disable-high-entropy-va "SHELL:-Xlinker --stack -Xlinker 16777216,16777216")
    # theatrhythm-setup.exe (setup/): sets up a release folder (tools/package.py) from a player's dump.
    # Linked statically, so it starts without the MSYS2 DLLs.
    add_executable(theatrhythm-setup WIN32 ../setup/main.cpp ../setup/setup.cpp ../setup/setup.rc)
    target_compile_definitions(theatrhythm-setup PRIVATE UNICODE _UNICODE)
    target_link_options(theatrhythm-setup PRIVATE -municode -static)
    target_link_libraries(theatrhythm-setup PRIVATE comctl32 ole32 shell32 uuid)
else()
    # Not PIE: host heap objects handed to the guest stay in the brk heap, below 1 TiB (probe.c main).
    set_target_properties(theatrhythm-fbl PROPERTIES POSITION_INDEPENDENT_CODE OFF)
    target_link_options(theatrhythm-fbl PRIVATE -no-pie)
    target_link_libraries(theatrhythm-fbl PRIVATE ${CMAKE_DL_LIBS} m)
    # theatrhythm-setup (setup/): the same install, asking in a terminal or with the desktop's dialogs (SDL).
    add_executable(theatrhythm-setup ../setup/main_linux.cpp ../setup/setup.cpp)
    target_link_libraries(theatrhythm-setup PRIVATE PkgConfig::SDL3)
endif()
# theatrhythm: runtime unit tests (ninja -C out/gpu mp3-test videodec-test services-test).
add_executable(mp3-test EXCLUDE_FROM_ALL ../tests/test_mp3.c ../src/runtime_mp3.c ${THEATRHYTHM_COMPAT})
add_executable(videodec-test EXCLUDE_FROM_ALL ../tests/test_videodec.c ${THEATRHYTHM_COMPAT})
add_executable(services-test EXCLUDE_FROM_ALL ../tests/test_services.c ${THEATRHYTHM_COMPAT})
foreach(test mp3-test videodec-test services-test)
    set_target_properties(${test} PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    target_compile_options(${test} PRIVATE -Wall -Wextra -Werror)
    target_include_directories(${test} PRIVATE .. ../src)
    target_link_libraries(${test} PRIVATE atrac9 PkgConfig::FFMPEG ${THEATRHYTHM_COMPAT_LIBS} -pthread)
endforeach()
add_executable(audio-test EXCLUDE_FROM_ALL ../tests/test_audio.c ${THEATRHYTHM_COMPAT})
set_target_properties(audio-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(audio-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(audio-test PRIVATE .. ../src)
target_link_libraries(audio-test PRIVATE PkgConfig::SDL3 ${THEATRHYTHM_COMPAT_LIBS} -pthread)
add_executable(pad-test EXCLUDE_FROM_ALL ../tests/test_pad.c ${THEATRHYTHM_COMPAT})
set_target_properties(pad-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(pad-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(pad-test PRIVATE .. ../src)
target_link_libraries(pad-test PRIVATE PkgConfig::SDL3 ${THEATRHYTHM_COMPAT_LIBS} -pthread)
add_executable(audio-downmix-test EXCLUDE_FROM_ALL ../tests/test_audio_downmix.c ${THEATRHYTHM_COMPAT})
set_target_properties(audio-downmix-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(audio-downmix-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(audio-downmix-test PRIVATE .. ../src)
target_link_libraries(audio-downmix-test PRIVATE PkgConfig::SDL3 ${THEATRHYTHM_COMPAT_LIBS} -pthread)
add_executable(cpu-check-test EXCLUDE_FROM_ALL ../tests/test_cpu_check.c)
set_target_properties(cpu-check-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(cpu-check-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(cpu-check-test PRIVATE .. ../src)
# The test runs the real instructions as its oracle.
add_executable(isa-emulate-test EXCLUDE_FROM_ALL ../tests/test_isa_emulate.c ../src/isa_emulate.c)
set_source_files_properties(../tests/test_isa_emulate.c PROPERTIES COMPILE_OPTIONS "-mbmi;-mlzcnt;-mmovbe")
set_target_properties(isa-emulate-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(isa-emulate-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(isa-emulate-test PRIVATE .. ../src)
# Stubs and patched sites run for real in an RWX region; the fixture path is the first argument.
add_executable(isa-stub-test EXCLUDE_FROM_ALL ../tests/test_isa_stub.c ../src/isa_rewrite.c ../src/isa_emulate.c)
set_source_files_properties(../tests/test_isa_stub.c PROPERTIES COMPILE_OPTIONS "-mbmi;-mlzcnt")
set_target_properties(isa-stub-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(isa-stub-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(isa-stub-test PRIVATE .. ../src)
target_link_libraries(isa-stub-test PRIVATE ${THEATRHYTHM_COMPAT_LIBS} -pthread)
add_executable(tsc-clock-test EXCLUDE_FROM_ALL ../tests/test_tsc_clock.c ${THEATRHYTHM_COMPAT})
set_target_properties(tsc-clock-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(tsc-clock-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(tsc-clock-test PRIVATE .. ../src)
target_link_libraries(tsc-clock-test PRIVATE ${THEATRHYTHM_COMPAT_LIBS} -pthread)
if (WIN32)
    # Memory another program holds in the PS4 range at start-up.
    add_executable(memory-reserve-test EXCLUDE_FROM_ALL ../tests/test_memory_reserve.c ../src/compat_win.c)
    set_target_properties(memory-reserve-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    target_compile_options(memory-reserve-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    target_include_directories(memory-reserve-test PRIVATE .. ../src)
    target_link_libraries(memory-reserve-test PRIVATE onecore bcrypt -pthread)
    target_link_options(memory-reserve-test PRIVATE -Wl,--disable-high-entropy-va)
endif()

# Explicit test targets; neither launches the game nor needs a Vulkan device.
add_executable(pipeline-binary-cache-test EXCLUDE_FROM_ALL ../tests/test_pipeline_binary_cache.cpp
    ../tests/test_gpu_runtime_stubs.cpp)
target_compile_definitions(pipeline-binary-cache-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pipeline-binary-cache-test PRIVATE shim shadps4)
target_link_libraries(pipeline-binary-cache-test PRIVATE theatrhythm_gpu)
target_compile_options(pipeline-binary-cache-test PRIVATE -fPIC)
if (WIN32)
    target_sources(pipeline-binary-cache-test PRIVATE ../src/compat_win.c)
    target_compile_definitions(pipeline-binary-cache-test PRIVATE sigjmp_buf=theatrhythm_jmp_buf)
    target_compile_options(pipeline-binary-cache-test PRIVATE
        -include ${CMAKE_CURRENT_SOURCE_DIR}/../src/compat_win.h)
    target_link_libraries(pipeline-binary-cache-test PRIVATE onecore)
endif()
add_executable(pipeline-binary-initialization-test EXCLUDE_FROM_ALL
    ../tests/test_pipeline_binary_initialization.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_compile_definitions(pipeline-binary-initialization-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pipeline-binary-initialization-test PRIVATE shim shadps4)
target_link_libraries(pipeline-binary-initialization-test PRIVATE theatrhythm_gpu)
target_compile_options(pipeline-binary-initialization-test PRIVATE -fPIC)
if (WIN32)
    target_sources(pipeline-binary-initialization-test PRIVATE ../src/compat_win.c)
    target_compile_definitions(pipeline-binary-initialization-test PRIVATE sigjmp_buf=theatrhythm_jmp_buf)
    target_compile_options(pipeline-binary-initialization-test PRIVATE
        -include ${CMAKE_CURRENT_SOURCE_DIR}/../src/compat_win.h)
    target_link_libraries(pipeline-binary-initialization-test PRIVATE onecore)
endif()
set_source_files_properties(../tests/test_pipeline_binary_cache.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
add_executable(png-decode-test EXCLUDE_FROM_ALL ../tests/test_png_decode.cpp shim/theatrhythm_png.cpp)
target_compile_options(png-decode-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_link_libraries(png-decode-test PRIVATE PkgConfig::FFMPEG)
add_executable(aliasing-test EXCLUDE_FROM_ALL ../tests/test_aliasing.cpp
    shadps4/video_core/texture_cache/aliasing.cpp shadps4/video_core/texture_cache/host_compatibility.cpp)
target_compile_options(aliasing-test PRIVATE -UNDEBUG)
target_include_directories(aliasing-test PRIVATE shim shadps4)
target_link_libraries(aliasing-test PRIVATE Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum)
add_executable(blend-rewrite-test EXCLUDE_FROM_ALL ../tests/test_blend_rewrite.cpp)
target_compile_options(blend-rewrite-test PRIVATE -UNDEBUG)
target_include_directories(blend-rewrite-test PRIVATE shim shadps4)
target_link_libraries(blend-rewrite-test PRIVATE Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum)
add_executable(depth-stencil-state-test EXCLUDE_FROM_ALL ../tests/test_depth_stencil_state.cpp)
add_executable(async-compiler-test EXCLUDE_FROM_ALL ../tests/test_async_compiler.cpp)
target_compile_options(async-compiler-test PRIVATE -UNDEBUG)
target_include_directories(async-compiler-test PRIVATE shadps4)
add_executable(warmup-inbox-test EXCLUDE_FROM_ALL ../tests/test_warmup_inbox.cpp)
target_compile_options(warmup-inbox-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(warmup-inbox-test PRIVATE shadps4)
add_executable(wave-stats-test EXCLUDE_FROM_ALL ../tests/test_wave_stats.cpp)
target_compile_options(wave-stats-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(wave-stats-test PRIVATE shadps4)
add_executable(pass-census-test EXCLUDE_FROM_ALL ../tests/test_pass_census.cpp)
target_compile_options(pass-census-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(pass-census-test PRIVATE shadps4)
add_executable(frame-capture-test EXCLUDE_FROM_ALL ../tests/test_frame_capture.cpp
    shadps4/video_core/renderer_vulkan/vk_frame_capture.cpp)
target_compile_options(frame-capture-test PRIVATE -UNDEBUG)
target_include_directories(frame-capture-test PRIVATE shim shadps4)
target_link_libraries(frame-capture-test PRIVATE Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum)
add_executable(amd-paths-test EXCLUDE_FROM_ALL ../tests/test_amd_paths.cpp)
target_compile_options(amd-paths-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(amd-paths-test PRIVATE shadps4)
add_executable(no-msaa-test EXCLUDE_FROM_ALL ../tests/test_no_msaa.cpp
    shadps4/video_core/amdgpu/pixel_format.cpp)
target_compile_options(no-msaa-test PRIVATE -UNDEBUG)
target_include_directories(no-msaa-test PRIVATE shim shadps4)
target_link_libraries(no-msaa-test PRIVATE Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum)
add_executable(async-pipeline-cache-test EXCLUDE_FROM_ALL ../tests/test_async_pipeline_cache.cpp)
target_compile_options(async-pipeline-cache-test PRIVATE -UNDEBUG)
target_include_directories(async-pipeline-cache-test PRIVATE shadps4)
target_link_libraries(async-pipeline-cache-test PRIVATE Vulkan::Vulkan)
add_executable(async-fetch-snapshot-test EXCLUDE_FROM_ALL ../tests/test_async_fetch_snapshot.cpp
    shadps4/shader_recompiler/frontend/fetch_shader.cpp
    shadps4/shader_recompiler/frontend/decode.cpp
    shadps4/shader_recompiler/frontend/format.cpp)
target_compile_options(async-fetch-snapshot-test PRIVATE -UNDEBUG)
target_include_directories(async-fetch-snapshot-test PRIVATE shim shadps4)
target_link_libraries(async-fetch-snapshot-test PRIVATE Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum)
target_compile_options(depth-stencil-state-test PRIVATE -UNDEBUG)
target_include_directories(depth-stencil-state-test PRIVATE shim shadps4)
target_link_libraries(depth-stencil-state-test PRIVATE Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum)

# The test's Vulkan-Hpp inline methods have a different set of platform extensions
# from vk_platform.cpp. Keep them local and use an explicit dispatcher in the test.


add_executable(readback-queue-test EXCLUDE_FROM_ALL ../tests/test_readback_queue.cpp)
target_include_directories(readback-queue-test PRIVATE shadps4)
target_compile_options(readback-queue-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_executable(wait-diagnostics-test EXCLUDE_FROM_ALL ../tests/test_wait_diagnostics.cpp)
target_include_directories(wait-diagnostics-test PRIVATE shadps4)
target_compile_options(wait-diagnostics-test PRIVATE -UNDEBUG)
add_executable(readback-sources-test EXCLUDE_FROM_ALL ../tests/test_readback_sources.cpp)
target_include_directories(readback-sources-test PRIVATE shadps4)
target_compile_options(readback-sources-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_executable(page-state-test EXCLUDE_FROM_ALL ../tests/test_page_state.cpp)
target_include_directories(page-state-test PRIVATE ../tests/stubs shim shadps4)
target_link_libraries(page-state-test PRIVATE Boost::headers)
target_compile_options(page-state-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)



add_executable(swapchain-focus-test EXCLUDE_FROM_ALL ../tests/test_swapchain_focus.cpp
    shim/window.cpp shim/theatrhythm_settings.cpp)
target_include_directories(swapchain-focus-test PRIVATE shim shadps4)
target_link_libraries(swapchain-focus-test PRIVATE PkgConfig::SDL3 Vulkan::Vulkan fmt::fmt)
target_compile_options(swapchain-focus-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
