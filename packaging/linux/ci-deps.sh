#!/usr/bin/env bash
# Linux build dependencies on Ubuntu 24.04 (.github/workflows/build.yml). Its glibc (2.39) is the
# oldest one players can have: SteamOS, Bazzite/Fedora, Debian 13 and Ubuntu 24.04 or newer. The DLC
# maker links libvorbis/libogg in, and its ffmpeg (tools/dlcmaker/build-ffmpeg.sh) libx264, libdav1d and zlib.
# What 24.04 lacks or ships too old is built into /usr/local: Vulkan-Headers, VMA, SDL3.
set -euo pipefail
sudo=$([[ $(id -u) == 0 ]] || echo sudo)
export DEBIAN_FRONTEND=noninteractive
$sudo apt-get update
$sudo apt-get install -y --no-install-recommends \
    clang-18 lld-18 llvm-18 libstdc++-14-dev cmake ninja-build pkg-config git curl ca-certificates file patchelf \
    libvulkan-dev glslang-tools libboost-dev libfmt-dev libxxhash-dev libzydis-dev robin-map-dev \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxfixes-dev \
    libxinerama-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libdrm-dev libgbm-dev \
    libegl-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev libibus-1.0-dev \
    python3 nasm make libvorbis-dev libogg-dev libx264-dev libdav1d-dev zlib1g-dev
# No cc on a fresh install (the CI image has one): what is built below uses clang-18 too, and
# build.sh then needs CC=clang-18 CXX=clang++-18.
export CC=${CC:-clang-18} CXX=${CXX:-clang++-18}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# Vulkan-Headers (and Vulkan-Hpp): 24.04 has 1.3.275, the renderer needs 1.4 types.
VULKAN_HEADERS=${VULKAN_HEADERS:-vulkan-sdk-1.4.357.0}
git clone -q --depth 1 -b "$VULKAN_HEADERS" https://github.com/KhronosGroup/Vulkan-Headers "$work/vulkan-headers"
cmake -S "$work/vulkan-headers" -B "$work/vulkan-headers/build" -G Ninja -DCMAKE_INSTALL_PREFIX=/usr/local >/dev/null
$sudo cmake --install "$work/vulkan-headers/build" >/dev/null
# Vulkan Memory Allocator: header only, not packaged in 24.04.
VMA=${VMA:-v3.1.0}
$sudo curl -fsSL -o /usr/local/include/vk_mem_alloc.h \
    "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/$VMA/include/vk_mem_alloc.h"
# SDL3: not packaged in 24.04.
SDL=${SDL:-3.4.16}
curl -fsSL "https://github.com/libsdl-org/SDL/releases/download/release-$SDL/SDL3-$SDL.tar.gz" | tar xz -C "$work"
cmake -S "$work/SDL3-$SDL" -B "$work/sdl" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF >/dev/null
ninja -C "$work/sdl" >/dev/null
$sudo ninja -C "$work/sdl" install >/dev/null
$sudo ldconfig
