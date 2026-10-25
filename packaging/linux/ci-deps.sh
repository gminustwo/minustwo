#!/usr/bin/env bash
# Build dependencies of the Linux AppImage on Ubuntu 24.04 (from SpartanPort's packaging/linux/ci-deps.sh).
# Its glibc (2.39) is the oldest one players can have: SteamOS, Bazzite/Fedora, Debian 13 and Ubuntu
# 24.04 or newer. What 24.04 lacks or ships too old is built into $PREFIX: Vulkan-Headers 1.4, VMA, SDL3.
#   PREFIX=/usr/local (default; needs root) or a folder of your own, e.g. PREFIX=$HOME/.local/minustwo-deps
#   SKIP_APT=1: the Ubuntu packages are installed already (no root needed then with a home PREFIX).
# build.sh finds a home PREFIX through MT_DEPS=$PREFIX. GCC 14 is used through CC/CXX, the system's
# default compiler is left alone.
set -euo pipefail
PREFIX=${PREFIX:-/usr/local}
sudo=$([[ $(id -u) == 0 ]] || echo sudo)
if [[ -z ${SKIP_APT:-} ]]; then
    export DEBIAN_FRONTEND=noninteractive
    $sudo apt-get update
    $sudo apt-get install -y --no-install-recommends \
        build-essential g++-14 gcc-14 cmake ninja-build pkg-config git curl ca-certificates file patchelf binutils \
        libvulkan-dev glslang-tools libboost-dev libfmt-dev libxxhash-dev libzydis-dev robin-map-dev \
        libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libfreetype-dev \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxfixes-dev \
        libxinerama-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libdrm-dev libgbm-dev \
        libegl-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev libibus-1.0-dev \
        python3 python3-venv python3-tk python3-dev
fi
mkdir -p "$PREFIX" 2>/dev/null || $sudo mkdir -p "$PREFIX"
install=()  # root for the installs only when PREFIX is not ours (/usr/local)
[[ -w $PREFIX ]] || install=($sudo)

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# Vulkan-Headers (and Vulkan-Hpp): 24.04 has 1.3.275, the renderer needs 1.4 types.
VULKAN_HEADERS=${VULKAN_HEADERS:-vulkan-sdk-1.4.357.0}
git clone -q --depth 1 -b "$VULKAN_HEADERS" https://github.com/KhronosGroup/Vulkan-Headers "$work/vulkan-headers"
cmake -S "$work/vulkan-headers" -B "$work/vulkan-headers/build" -G Ninja -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
"${install[@]}" cmake --install "$work/vulkan-headers/build" >/dev/null
# Vulkan Memory Allocator: header only, not packaged in 24.04.
VMA=${VMA:-v3.1.0}
"${install[@]}" mkdir -p "$PREFIX/include"
"${install[@]}" curl -fsSL -o "$PREFIX/include/vk_mem_alloc.h" \
    "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/$VMA/include/vk_mem_alloc.h"
# SDL3: not packaged in 24.04.
SDL=${SDL:-3.4.16}
curl -fsSL "https://github.com/libsdl-org/SDL/releases/download/release-$SDL/SDL3-$SDL.tar.gz" | tar xz -C "$work"
cmake -S "$work/SDL3-$SDL" -B "$work/sdl" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF >/dev/null
ninja -C "$work/sdl" >/dev/null
"${install[@]}" ninja -C "$work/sdl" install >/dev/null
[[ $PREFIX == /usr/local ]] && $sudo ldconfig

# The launcher is frozen with PyInstaller (packaging/linux/build.sh): venv at $VENV.
VENV=${VENV:-$PWD/out/venv}
python3 -m venv "$VENV"
"$VENV/bin/pip" install -q --no-cache-dir pyinstaller pillow iced-x86==1.21.0
echo "Dependencies in $PREFIX; launcher tools in $VENV"
