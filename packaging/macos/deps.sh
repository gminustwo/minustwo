#!/usr/bin/env bash
# x86-64 libraries for the macOS build (build.sh runs this before the first build). The game's code
# is x86-64 and runs under Rosetta 2, so minustwo and every library it loads are x86-64 as well, and
# Homebrew on Apple silicon only has arm64 ones. They are built from source into out/macos-deps as
# shared libraries with @rpath install names (packaging/macos/package.sh copies them into the app),
# the Vulkan driver (KosmicKrisp) too. Each one is built once: its stamp in out/macos-deps/stamps
# names the version (and +N when this script changed how it is built), so raising a version below
# rebuilds only that library.
# Needs Xcode (or its command line tools), git, curl, python3 and Homebrew's cmake, ninja, pkgconf,
# nasm, llvm, spirv-tools and spirv-llvm-translator (the last three for KosmicKrisp's compiler).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
root=$PWD
prefix=$root/out/macos-deps
src=$prefix/src
stamps=$prefix/stamps
mkdir -p "$src" "$stamps"
for tool in git curl cmake ninja pkg-config nasm clang; do
    command -v "$tool" >/dev/null || { echo "Need $tool (brew install cmake ninja pkgconf nasm glslang)." >&2; exit 1; }
done
jobs=$(sysctl -n hw.ncpu)
# minustwo runs on Rosetta 2 with AVX (macOS 15 and newer); the libraries ask for less.
export MACOSX_DEPLOYMENT_TARGET=13.0
# Nothing from the arm64 Homebrew prefix may end up in an x86-64 library.
export PKG_CONFIG_LIBDIR=$prefix/lib/pkgconfig
unset PKG_CONFIG_PATH CPATH LIBRARY_PATH

VULKAN=v1.4.366
# shadexternals/mesa-kosmickrisp, the commit shadPS4 b4e7ae7 (the vendored video core) builds.
KOSMICKRISP=3af112680499cc5eaf519007404a7366679e1c7f
SDL=release-3.4.16
FREETYPE=VER-2-14-3
FFMPEG=n8.1.3
FMT=12.1.0
ROBIN_MAP=v1.4.1
XXHASH=v0.8.3
ZYDIS=v4.1.1
VMA=v3.3.0
BOOST=1.89.0

done_already() { [[ -f $stamps/$1-$2 ]]; }
finish() { rm -f "$stamps/$1"-*; touch "$stamps/$1-$2"; echo "macOS dependencies: $1 $2"; }
# fetch name repository tag: a shallow clone of the tag (submodules included) in src/name.
fetch() {
    local name=$1 repository=$2 tag=$3
    if [[ -d $src/$name && $(cat "$src/$name.tag" 2>/dev/null) != "$tag" ]]; then rm -rf "${src:?}/$name"; fi
    if [[ ! -d $src/$name ]]; then
        git clone -q -c advice.detachedHead=false --depth 1 --branch "$tag" --recurse-submodules --shallow-submodules \
            "https://github.com/$repository.git" "$src/$name"
        echo "$tag" > "$src/$name.tag"
    fi
}
# cmake_install name source_dir [cmake options]: configure, build and install into the prefix.
cmake_install() {
    local name=$1 source=$2
    shift 2
    rm -rf "${src:?}/$name-build"
    cmake -S "$source" -B "$src/$name-build" -G Ninja -Wno-dev -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET \
        -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_INSTALL_NAME_DIR=@rpath -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" \
        -DCMAKE_FIND_FRAMEWORK=LAST -DCMAKE_POSITION_INDEPENDENT_CODE=ON "$@" > "$src/$name-build.log" 2>&1 ||
        { tail -30 "$src/$name-build.log" >&2; echo "macOS dependencies: configuring $name failed" >&2; exit 1; }
    cmake --build "$src/$name-build" -j "$jobs" >> "$src/$name-build.log" 2>&1 &&
        cmake --install "$src/$name-build" >> "$src/$name-build.log" 2>&1 ||
        { tail -30 "$src/$name-build.log" >&2; echo "macOS dependencies: building $name failed" >&2; exit 1; }
}

if ! done_already vulkan-headers $VULKAN; then
    fetch vulkan-headers KhronosGroup/Vulkan-Headers $VULKAN
    cmake_install vulkan-headers "$src/vulkan-headers"
    finish vulkan-headers $VULKAN
fi
if ! done_already vulkan-loader $VULKAN+1; then
    # SYSCONFDIR=/etc: the system folder it searches besides the driver the game names (VK_DRIVER_FILES);
    # by default the prefix's etc as well, a path in the builder's home folder that players do not have.
    fetch vulkan-loader KhronosGroup/Vulkan-Loader $VULKAN
    cmake_install vulkan-loader "$src/vulkan-loader" -DBUILD_TESTS=OFF -DUPDATE_DEPS=OFF \
        -DVulkanHeaders_DIR="$prefix/share/cmake/VulkanHeaders" -DSYSCONFDIR=/etc
    finish vulkan-loader $VULKAN+1
fi
if ! done_already kosmickrisp $KOSMICKRISP; then
    # KosmicKrisp, LunarG's Mesa Vulkan driver on Metal (macOS 26 and newer), as shadPS4 uses it on
    # macOS: the renderer needs sparse buffers, robustBufferAccess2 and null descriptors, which
    # MoltenVK does not have. Its wrapper builds Mesa's compiler tools for this Mac first (Homebrew's
    # LLVM), then the x86-64 driver with them; meson and Mesa's Python modules come from a venv.
    llvm_config=$(brew --prefix llvm 2>/dev/null)/bin/llvm-config
    [[ -x $llvm_config ]] || { echo 'Need Homebrew LLVM for KosmicKrisp: brew install llvm spirv-tools spirv-llvm-translator' >&2; exit 1; }
    if [[ ! -x $prefix/python/bin/meson ]]; then
        python3 -m venv "$prefix/python"
        "$prefix/python/bin/pip" install -q --disable-pip-version-check 'meson>=1.4' mako pyyaml packaging certifi
    fi
    if [[ $(cat "$src/kosmickrisp.tag" 2>/dev/null) != "$KOSMICKRISP" ]]; then
        rm -rf "${src:?}/kosmickrisp" "${src:?}/kosmickrisp-build"
        git init -q "$src/kosmickrisp"
        git -C "$src/kosmickrisp" fetch -q --depth 1 https://github.com/shadexternals/mesa-kosmickrisp.git "$KOSMICKRISP"
        git -C "$src/kosmickrisp" -c advice.detachedHead=false checkout -q FETCH_HEAD
        git -C "$src/kosmickrisp" submodule -q update --init --depth 1
        echo "$KOSMICKRISP" > "$src/kosmickrisp.tag"
    fi
    (
        unset PKG_CONFIG_LIBDIR MACOSX_DEPLOYMENT_TARGET  # the tools link Homebrew's LLVM and SPIR-V libraries
        export PATH=$prefix/python/bin:$(dirname "$llvm_config"):$PATH
        # Meson downloads zlib for the x86-64 driver; python.org's Python has no certificates of its own.
        SSL_CERT_FILE=$("$prefix/python/bin/python" -m certifi)
        export SSL_CERT_FILE
        cmake -S "$src/kosmickrisp" -B "$src/kosmickrisp-build" -G Ninja -Wno-dev -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 -DKK_LLVM_CONFIG="$llvm_config"
        cmake --build "$src/kosmickrisp-build" -j "$jobs"
    ) > "$src/kosmickrisp-build.log" 2>&1 ||
        { grep -E 'error|ERROR|FAILED' "$src/kosmickrisp-build.log" | tail -30 >&2; echo 'macOS dependencies: building KosmicKrisp failed' >&2; exit 1; }
    mkdir -p "$prefix/lib" "$prefix/share/vulkan/icd.d"
    cp "$src/kosmickrisp-build/outputs/libvulkan_kosmickrisp.dylib" "$prefix/lib/"
    # Meson's zlib (downloaded for the driver) is not shipped: macOS has its own.
    install_name_tool -id @rpath/libvulkan_kosmickrisp.dylib -change @rpath/libz.1.dylib /usr/lib/libz.1.dylib \
        "$prefix/lib/libvulkan_kosmickrisp.dylib" 2>/dev/null
    codesign --force --sign - "$prefix/lib/libvulkan_kosmickrisp.dylib"
    sed 's#"library_path"[^,]*#"library_path": "../../../lib/libvulkan_kosmickrisp.dylib"#' \
        "$src/kosmickrisp-build/outputs/kosmickrisp_mesa_icd.json" > "$prefix/share/vulkan/icd.d/kosmickrisp_mesa_icd.json"
    finish kosmickrisp $KOSMICKRISP
fi
if ! done_already sdl3 $SDL; then
    fetch sdl3 libsdl-org/SDL $SDL
    cmake_install sdl3 "$src/sdl3" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF \
        -DSDL_EXAMPLES=OFF
    finish sdl3 $SDL
fi
if ! done_already freetype $FREETYPE; then
    fetch freetype freetype/freetype $FREETYPE
    cmake_install freetype "$src/freetype" -DBUILD_SHARED_LIBS=ON -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON \
        -DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
    finish freetype $FREETYPE
fi
if ! done_already ffmpeg $FFMPEG+1; then
    # What the movie player (MP4: H.264/HEVC with AAC), the video and MP3 decoders and the PNG
    # reader use. nasm assembles the x86 SIMD code, which Rosetta translates like the rest.
    # Neither the assert paths nor the configure line the libraries keep (avcodec_configuration())
    # hold the build folder, which is in the builder's home folder: both start at the repository.
    fetch ffmpeg FFmpeg/FFmpeg $FFMPEG
    rm -rf "${src:?}/ffmpeg-build" && mkdir -p "$src/ffmpeg-build"
    (
        cd "$src/ffmpeg-build"
        "$src/ffmpeg/configure" --prefix="$prefix" --enable-shared --disable-static --install-name-dir=@rpath \
            --arch=x86_64 --target-os=darwin --enable-cross-compile --cc="clang -arch x86_64" \
            --extra-cflags="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -ffile-prefix-map=$root/=" \
            --extra-ldflags="-arch x86_64 -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" --x86asmexe=nasm \
            --disable-programs --disable-doc --disable-network --disable-autodetect --enable-pthreads --enable-zlib \
            --disable-everything --disable-avdevice --disable-avfilter --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-swresample \
            --enable-decoder=h264,hevc,aac,mp3,mp3float,png --enable-parser=h264,hevc,aac,mpegaudio,png \
            --enable-demuxer=mov,mp3,aac,h264,hevc --enable-protocol=file
        sed -i '' "s#$root/##g" config.h
        make -j "$jobs"
        make install
    ) > "$src/ffmpeg-build.log" 2>&1 || { tail -30 "$src/ffmpeg-build.log" >&2; echo 'macOS dependencies: building FFmpeg failed' >&2; exit 1; }
    finish ffmpeg $FFMPEG+1
fi
if ! done_already fmt $FMT; then
    fetch fmt fmtlib/fmt $FMT
    cmake_install fmt "$src/fmt" -DFMT_TEST=OFF -DFMT_DOC=OFF -DBUILD_SHARED_LIBS=OFF
    finish fmt $FMT
fi
if ! done_already robin-map $ROBIN_MAP; then
    fetch robin-map Tessil/robin-map $ROBIN_MAP
    cmake_install robin-map "$src/robin-map"
    finish robin-map $ROBIN_MAP
fi
if ! done_already xxhash $XXHASH; then
    fetch xxhash Cyan4973/xxHash $XXHASH
    cmake_install xxhash "$src/xxhash/cmake_unofficial" -DXXHASH_BUILD_XXHSUM=OFF -DBUILD_SHARED_LIBS=OFF
    finish xxhash $XXHASH
fi
if ! done_already zydis $ZYDIS; then
    # Zycore (its submodule) is also installed on its own: Zydis' package configuration needs it.
    fetch zydis zyantific/zydis $ZYDIS
    cmake_install zycore "$src/zydis/dependencies/zycore" -DZYCORE_BUILD_SHARED_LIB=OFF
    cmake_install zydis "$src/zydis" -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_EXAMPLES=OFF -DZYDIS_BUILD_DOXYGEN=OFF \
        -DZYDIS_BUILD_SHARED_LIB=OFF -DZYCORE_BUILD_SHARED_LIB=OFF
    finish zydis $ZYDIS
fi
if ! done_already vma $VMA; then
    fetch vma GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator $VMA
    cmake_install vma "$src/vma"
    finish vma $VMA
fi
if ! done_already boost $BOOST; then
    # Header-only use (container, icl, intrusive, pool, asio): the headers of the release.
    archive=boost_${BOOST//./_}
    rm -rf "${src:?}/$archive" "$prefix/include/boost"
    curl -fsSL "https://archives.boost.io/release/$BOOST/source/$archive.tar.bz2" | tar -xj -C "$src" "$archive/boost"
    mkdir -p "$prefix/include"
    mv "$src/$archive/boost" "$prefix/include/boost"
    rm -rf "${src:?}/$archive"
    finish boost $BOOST
fi
