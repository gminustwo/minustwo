#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
mkdir -p out
# Windows: MSYS2 CLANG64 (packages in packaging/windows/README.md). The GPU library is static
# there and CMake links minustwo.exe as well; no PGO (GCC-only flags), LTO off by default.
case $(uname -s) in
MINGW*|MSYS*)
    # shellcheck source=msys2-env.sh
    source ./msys2-env.sh
    for tool in clang cmake ninja pkg-config; do
        command -v "$tool" >/dev/null || { echo "Need $tool from MSYS2 CLANG64 (packaging/windows/README.md)." >&2; exit 1; }
    done
    if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ||
          ! -f gpu/third_party/xbyak/xbyak/xbyak.h ]]; then
        git submodule update --init --recursive
    fi
    for patch in gpu/patches/fsr-vulkan/*.patch; do
        if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
            git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
        fi
    done
    cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMT_PGO=off \
        -DMT_LTO="${MT_LTO:-OFF}" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
    echo "GPU library and loader: LTO ${MT_LTO:-OFF}"
    if ! ninja -C out/gpu minustwo minustwo-gpu-capabilities minustwo-play > out/gpu-build.log 2>&1; then
        grep -v '^\[' out/gpu-build.log | tail -40 >&2
        echo 'Build failed (full log: out/gpu-build.log)' >&2; exit 1
    fi
    cp out/gpu/minustwo.exe out/gpu/minustwo-gpu-capabilities.exe out/gpu/minustwo-play.exe out/
    echo "Built $PWD/out/minustwo.exe"
    exit 0
    ;;
Darwin)
    # macOS: x86-64 (the game's code runs under Rosetta 2 on Apple silicon), with the libraries
    # packaging/macos/deps.sh builds into out/macos-deps; the GPU library is static as on Windows.
    # Rosetta 2 runs AVX from macOS 15; the Vulkan driver (KosmicKrisp) needs macOS 26.
    for tool in clang cmake ninja pkg-config nasm glslangValidator; do
        command -v "$tool" >/dev/null || { echo "Need $tool: xcode-select --install; brew install cmake ninja pkgconf nasm glslang llvm spirv-tools spirv-llvm-translator" >&2; exit 1; }
    done
    bash packaging/macos/deps.sh
    deps=$PWD/out/macos-deps
    export PKG_CONFIG_LIBDIR=$deps/lib/pkgconfig
    unset PKG_CONFIG_PATH
    if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ||
          ! -f gpu/third_party/xbyak/xbyak/xbyak.h ]]; then
        git submodule update --init --recursive
    fi
    for patch in gpu/patches/fsr-vulkan/*.patch; do
        if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
            git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
        fi
    done
    cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMT_PGO=off -DMT_LTO="${MT_LTO:-OFF}" \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_OSX_ARCHITECTURES=x86_64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DCMAKE_PREFIX_PATH="$deps" -DCMAKE_FIND_FRAMEWORK=LAST \
        -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" -DMT_MACOS_DEPS="$deps" \
        -DPKG_CONFIG_EXECUTABLE="$(command -v pkg-config)" -DGLSLANG_COMPILER="$(command -v glslangValidator)" >/dev/null
    echo "GPU library and loader: LTO ${MT_LTO:-OFF}"
    if ! ninja -C out/gpu minustwo minustwo-gpu-capabilities > out/gpu-build.log 2>&1; then
        grep -v '^\[' out/gpu-build.log | tail -40 >&2
        echo 'Build failed (full log: out/gpu-build.log)' >&2; exit 1
    fi
    cp out/gpu/minustwo out/gpu/minustwo-gpu-capabilities out/
    # The Vulkan loader finds KosmicKrisp through the manifest beside the executable (vk_platform.cpp).
    sed 's#"library_path"[^,]*#"library_path": "macos-deps/lib/libvulkan_kosmickrisp.dylib"#' \
        "$deps/share/vulkan/icd.d/kosmickrisp_mesa_icd.json" > out/kosmickrisp_mesa_icd.json
    echo "Built $PWD/out/minustwo"
    exit 0
    ;;
esac
# MT_DEPS: a folder with what the distribution lacks (packaging/linux/ci-deps.sh): SDL3, Vulkan 1.4
# headers, VMA. Its headers come before the system's.
if [[ -n ${MT_DEPS:-} ]]; then
    export PKG_CONFIG_PATH="$MT_DEPS/lib/pkgconfig:$MT_DEPS/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export CMAKE_PREFIX_PATH="$MT_DEPS${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
    export CPATH="$MT_DEPS/include${CPATH:+:$CPATH}"
    export LIBRARY_PATH="$MT_DEPS/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
fi
# GCC 14 when installed (Ubuntu 24.04's default is 13): the renderer's C++23 needs it.
if [[ -z ${CC:-} ]] && command -v gcc-14 >/dev/null; then
    export CC=gcc-14 CXX=${CXX:-g++-14}
fi
if [[ -z ${CC:-} ]]; then
    CC=$(command -v cc || command -v gcc || true)
    if [[ -z $CC ]]; then
        for candidate in /nix/store/*-gcc-wrapper-*/bin/gcc; do
            if [[ -x $candidate ]]; then CC=$candidate; break; fi
        done
    fi
fi
if [[ -z ${CC:-} ]]; then echo 'Install GCC/Clang or set CC.' >&2; exit 1; fi
# Dependencies come from pkg-config (Vulkan loader/headers, SDL3). On NixOS the
# environment is provided by shell.nix; re-enter it automatically if needed.
if ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3 && command -v cmake >/dev/null && command -v ninja >/dev/null; }; then
    if [[ -z ${MT_IN_NIX_SHELL:-} ]] && command -v nix-shell >/dev/null; then
        exec env MT_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash build.sh $*"
    fi
    echo 'Need pkg-config with vulkan and sdl3, cmake and ninja (see shell.nix).' >&2; exit 1
fi
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3 freetype2 libavcodec libavutil libswscale)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3 freetype2 libavcodec libavutil libswscale)"
# GPU library (shadPS4 video core + drivers), built by CMake into out/gpu/libmtgpu.so.
# MT_PGO: generate (instrumented build that writes pgo/ while the game runs), use, off.
# Default: use the profile in pgo/ when there is one. MT_LTO=OFF disables link-time optimization.
pgo=${MT_PGO:-}
if [[ -z $pgo ]]; then
    if [[ -n $(find pgo -name '*.gcda' -print -quit 2>/dev/null) ]]; then pgo=use; else pgo=off; fi
fi
mkdir -p pgo
# Submodules (git clone --recursive, or: git submodule update --init) and this port's changes
# to FSR-Vulkan (gpu/patches/fsr-vulkan), applied to its working tree once.
if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ]]; then
    git submodule update --init --recursive
fi
for patch in gpu/patches/fsr-vulkan/*.patch; do
    if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
        git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
    fi
done
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMT_PGO="$pgo" \
    -DMT_LTO="${MT_LTO:-ON}" -DMT_PGO_DIR="$PWD/pgo" >/dev/null
echo "GPU library: PGO $pgo, LTO ${MT_LTO:-ON}"
# A failed GPU build must stop here: an older libmtgpu.so would otherwise be used silently.
if ! ninja -C out/gpu mtgpu > out/gpu-build.log 2>&1; then
    grep -v '^\[' out/gpu-build.log | tail -40 >&2
    echo 'GPU library build failed (full log: out/gpu-build.log)' >&2; exit 1
fi
# $ORIGIN/gpu: packaged copies keep the library next to the binary without patching it.
gpu=(-Lout/gpu -lmtgpu -Wl,-rpath,'$ORIGIN/gpu' -Wl,-rpath,"$PWD/out/gpu" -rdynamic)
runtime=(src/runtime*.c)
# Third-party decoders: compiled once, without this project's -Werror policy.
atrac9=(third_party/LibAtrac9/C/src/*.c)
if [[ ! -f out/libatrac9.a || -n $(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c') ]]; then
    rm -rf out/atrac9 && mkdir -p out/atrac9
    for source in "${atrac9[@]}"; do "$CC" -std=c99 -O2 -g -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"; done
    ar rcs out/libatrac9.a out/atrac9/*.o
fi
# isa_emulate runs inside guest code: no XMM/YMM.
"$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -mgeneral-regs-only -c src/isa_emulate.c -o out/isa_emulate.o
"$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -no-pie "${includes[@]}" -I. -Isrc src/probe.c "${runtime[@]}" src/cpu_check.c src/vulkan_smoke.c out/isa_emulate.o src/isa_rewrite.c out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/minustwo
echo "Built $PWD/out/minustwo"
# GPU check for run.sh (live_resolution=auto): links only the Vulkan loader.
"$CC" -std=c11 -O2 -Wall -Wextra -Werror -Igpu/shadps4 -Isrc tools/gpu_capabilities.c "${libraries[@]}" -o out/minustwo-gpu-capabilities
if [[ ${1:-} == --test ]]; then
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_pad.c "${libraries[@]}" -o out/pad-test
    out/pad-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_runtime.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/runtime-test
    out/runtime-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_audio.c "${libraries[@]}" -o out/audio-test
    out/audio-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -Isrc tests/test_file_mods.c -o out/file-mods-test
    out/file-mods-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_sema.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/sema-test
    out/sema-test
    "$CC" -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -I. -Isrc tests/test_content.c src/runtime_content.c -o out/content-test
    out/content-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_mp3.c src/runtime_mp3.c out/libatrac9.a -lm "${libraries[@]}" -o out/mp3-test
    out/mp3-test tests/data/sine.mp3
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_videodec.c "${libraries[@]}" -o out/videodec-test
    out/videodec-test tests/data/tiny.h264
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_services.c -o out/services-test
    out/services-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_cheats.c -o out/cheats-test
    out/cheats-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_label_wait.c -o out/label-wait-test
    out/label-wait-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -mbmi -mlzcnt -mmovbe -I. -Isrc tests/test_isa_emulate.c out/isa_emulate.o -o out/isa-emulate-test
    out/isa-emulate-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -c src/isa_rewrite.c -I. -Isrc -o out/isa_rewrite.o
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -mbmi -mlzcnt -I. -Isrc tests/test_isa_stub.c out/isa_rewrite.o out/isa_emulate.o -o out/isa-stub-test
    out/isa-stub-test tests/data/isa_fixture.bin
    ninja -C out/gpu graphics-settings-test
    out/gpu/graphics-settings-test
fi
