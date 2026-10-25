#!/usr/bin/env bash
# Builds dist/minustwo-x86_64.AppImage (Steam Deck and other Linux PCs) from the Linux build: run
# `bash build.sh` in the repo root first (from SpartanPort's packaging/linux/build.sh). Libraries are
# bundled with their dependencies, except the ones that must come from the host (glibc, the Vulkan
# loader and drivers, X11/Wayland, GL, audio). The Tk launcher is frozen with PyInstaller, so players
# need no Python. Needs: ldd, patchelf, curl (appimagetool) and the venv of packaging/linux/ci-deps.sh
# (VENV, default out/venv). dist/minustwo-x86_64.AppImage.sha256 is written beside it for the
# launcher's updates.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
[[ -f out/minustwo && -f out/gpu/libmtgpu.so ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
VENV=${VENV:-$PWD/out/venv}
python=$VENV/bin/python3
[[ -x $python ]] || { echo "No venv at $VENV: packaging/linux/ci-deps.sh makes it" >&2; exit 1; }

appdir=out/AppDir
rm -rf "$appdir"
share=$appdir/usr/share/minustwo
mkdir -p "$share/bin/gpu" "$share/launcher" "$appdir/usr/lib"
# The executables without debug information (out/ keeps the symbols for crash reports).
for exe in minustwo minustwo-gpu-capabilities; do
    strip --strip-debug -o "$share/bin/$exe" "out/$exe"
done
strip --strip-debug -o "$share/bin/gpu/libmtgpu.so" out/gpu/libmtgpu.so
cp run.py LICENSE README.md packaging/linux/README-Linux.txt "$share/"
cp -r scripts patches "$share/"
install -m644 launcher/minustwo.png "$share/launcher/minustwo.png"

# The launcher also runs run.py (--run) and the preparation scripts (--script) itself, as on
# Windows: the standard modules those scripts import come along as hidden imports. PIL._tkinter_finder:
# Pillow's pictures in Tk on Linux (key art, trophy icons), a module PyInstaller does not find itself.
hidden=()
for module in argparse base64 collections hashlib json re shutil socket struct tempfile threading uuid \
              xml.etree.ElementTree urllib.request iced_x86 PIL._tkinter_finder; do
    hidden+=(--hidden-import "$module")
done
"$python" -m PyInstaller --noconfirm --clean --log-level WARN --windowed --copy-metadata pillow \
    --name minustwo-launcher --distpath out/pyi-dist --workpath out/pyi-work --specpath out/pyi-work \
    --paths "$PWD/scripts" "${hidden[@]}" "$PWD/launcher/minustwo_launcher.py"
cp -r out/pyi-dist/minustwo-launcher/. "$share/"
find "$share" -name __pycache__ -prune -exec rm -r {} +
install -m755 packaging/linux/AppRun "$appdir/AppRun"
install -m644 packaging/linux/minustwo.desktop "$appdir/minustwo.desktop"
install -m644 launcher/minustwo.png "$appdir/minustwo.png"

# Host-provided libraries: bundling any of these breaks drivers or the display connection.
excluded='^(ld-linux.*|libc|libm|libdl|librt|libpthread|libresolv|libutil|libnsl|libanl|libvulkan|libGL|libGLX|libGLdispatch|libEGL|libOpenGL|libgbm|libdrm.*|libX11|libX11-xcb|libxcb.*|libXau|libXdmcp|libXext|libXfixes|libXrender|libXi|libXcursor|libXrandr|libwayland-.*|libasound|libudev|libpulse.*|libfontconfig|libfreetype|libOpenCL|libva.*|libvdpau|libvpl|libnvidia-.*|libcuda|libz|libstdc\+\+|libgcc_s)\.so'
declare -A seen
bundle() {
    local elf=$1 line lib name
    while read -r line; do
        lib=$(awk '/=> \//{print $3}' <<<"$line")
        [[ -n $lib ]] || continue
        name=$(basename -- "$lib")
        [[ ${seen[$name]:-} ]] && continue
        seen[$name]=1
        [[ $name =~ $excluded ]] && continue
        install -m755 "$lib" "$appdir/usr/lib/$name"
        bundle "$lib"
    done < <(ldd "$elf")
}
bundle out/gpu/libmtgpu.so
bundle out/minustwo
bundle out/minustwo-gpu-capabilities
# libstdc++/libgcc of the build host (GCC 14), apart in usr/lib/cxx: AppRun uses them only when the
# system's are older, since the system's Vulkan driver loads into the game too and may need newer ones.
mkdir -p "$appdir/usr/lib/cxx"
for name in libstdc++.so.6 libgcc_s.so.1; do
    install -m755 "$(ldd out/gpu/libmtgpu.so | awk -v n="$name" '$1==n{print $3}')" "$appdir/usr/lib/cxx/$name"
done
find "$appdir/usr/lib" -name '*.so*' -exec strip --strip-unneeded {} + 2>/dev/null || true

# No game files and nothing a player made (saves, shader caches, settings, logs) may ship.
forbidden=$(find "$appdir" \( -iname eboot.bin -o -iname sce_module -o -iname sce_sys -o -iname savedata \
    -o -iname user -o -iname cache -o -iname '*.log' -o -iname minustwo.ini -o -iname settings.json \
    -o -iname mods.json -o -iname patches.json -o -iname fsr4_shaders \
    -o -iname '*.spv' -o -iname '*.pkg' -o -iname '*.sprx' -o -iname '*.prx' -o -iname '*.pac' \
    -o -iname '*.at9' -o -iname '*.mp4' -o -iname 'pic1.png' -o -iname 'icon0.png' \) -print)
if [[ -n $forbidden ]]; then
    echo "Package holds files that must not ship:" >&2
    echo "$forbidden" >&2
    exit 1
fi

# The newest glibc symbol in the bundle is the oldest glibc players can have; Ubuntu 24.04's 2.39 runs
# on SteamOS, Debian 13, Fedora and Ubuntu 24.04 or newer. MT_MAX_GLIBC raises it for local builds.
max_glibc=${MT_MAX_GLIBC:-2.39}
need=$(find "$appdir" -type f \( -name '*.so*' -o -perm -u+x \) -exec objdump -T {} + 2>/dev/null |
       grep -o 'GLIBC_2\.[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1 || true)
if [[ $(printf '%s\n' "$need" "$max_glibc" | sort -V | tail -1) != "$max_glibc" ]]; then
    echo "The bundle needs glibc $need, newer than $max_glibc: build on Ubuntu 24.04 or set MT_MAX_GLIBC" >&2
    exit 1
fi
echo "Minimum glibc: $need"

tool=${APPIMAGETOOL:-$(command -v appimagetool || true)}
if [[ -z $tool ]]; then
    tool=out/appimagetool-x86_64.AppImage
    [[ -x $tool ]] || {
        curl -fL -o "$tool" https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
        chmod +x "$tool"
    }
fi
mkdir -p dist
# Written aside and moved over: replacing a running AppImage in place fails ("Text file busy").
ARCH=x86_64 "$tool" --appimage-extract-and-run "$appdir" dist/minustwo-x86_64.AppImage.new
mv -f dist/minustwo-x86_64.AppImage.new dist/minustwo-x86_64.AppImage
(cd dist && sha256sum minustwo-x86_64.AppImage > minustwo-x86_64.AppImage.sha256)
ls -lh dist/minustwo-x86_64.AppImage
