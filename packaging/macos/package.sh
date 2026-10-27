#!/usr/bin/env bash
# Builds dist/minustwo-macos/ (and dist/minustwo-macos.zip with its .sha256): MinusTwo.app, the launcher
# frozen with PyInstaller so players need no Python, and Play MinusTwo.app, which starts the game with
# the saved settings (MinusTwo --play), as MinusTwo.exe and Play MinusTwo.exe do on Windows.
#
# MinusTwo.app/Contents/MacOS/MinusTwo is the launcher (native: it only runs Python); everything else of
# the port is in Contents/Resources, which the launcher takes as the port folder (port_folder()):
# run.py, scripts/, patches/, bin/minustwo (x86-64, run by Rosetta 2) with the KosmicKrisp manifest,
# and dylibs/ (SDL3, FFmpeg, FreeType, the Vulkan loader, KosmicKrisp). Saves, settings and the prepared
# game go to ~/Library/Application Support/MinusTwo. Run after `bash build.sh`. Freezing uses a
# python.org Python 3 with Tk (MACPYTHON overrides) and a private venv in out/pyenv-macos with
# PyInstaller and Pillow. The apps are signed ad hoc: they open on this Mac; a downloaded copy needs
# right-click > Open (no Developer ID, no notarization).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
deps=out/macos-deps
[[ -f out/minustwo && -f out/minustwo-gpu-capabilities && -f $deps/lib/libvulkan_kosmickrisp.dylib ]] ||
    { echo 'Build first: bash build.sh' >&2; exit 1; }

python=${MACPYTHON:-}
if [[ -z $python ]]; then
    for candidate in /Library/Frameworks/Python.framework/Versions/Current/bin/python3 "$(command -v python3 || true)"; do
        if [[ -x $candidate ]] && "$candidate" -c 'import tkinter' 2>/dev/null; then
            python=$candidate
            break
        fi
    done
fi
[[ -n $python ]] || { echo 'Need a Python 3 with Tk (python.org) or MACPYTHON=/path/to/python3' >&2; exit 1; }
venv=out/pyenv-macos
[[ -x $venv/bin/python ]] || "$python" -m venv "$venv"
"$venv/bin/python" -m pip install -q --disable-pip-version-check pyinstaller pillow
# iced_x86 serves the ISA rewrite (a CPU without AVX); Rosetta 2 runs AVX, so it is optional here.
hidden=()
if "$venv/bin/python" -m pip install -q --disable-pip-version-check iced-x86==1.21.0 2>/dev/null; then
    hidden+=(--hidden-import iced_x86)
fi
# The scripts run inside the launcher executable (--script): the standard modules they import come
# along. PIL._tkinter_finder: Pillow's pictures in Tk (key art, trophy icons).
for module in argparse base64 collections hashlib json re shutil socket struct tempfile threading uuid \
              xml.etree.ElementTree urllib.request PIL._tkinter_finder; do
    hidden+=(--hidden-import "$module")
done
version=$(sed -n "s/^VERSION = '\(.*\)'/\1/p" launcher/minustwo_launcher.py)

# The icon: the launcher's 256 px picture in the sizes macOS asks for.
iconset=out/pyi-work-macos/MinusTwo.iconset
rm -rf "$iconset"
mkdir -p "$iconset"
for size in 16 32 64 128 256; do
    sips -z "$size" "$size" launcher/minustwo.png --out "$iconset/icon_${size}x${size}.png" >/dev/null
    if (( size <= 128 )); then
        sips -z $((size * 2)) $((size * 2)) launcher/minustwo.png --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
    fi
done
iconutil -c icns -o out/pyi-work-macos/MinusTwo.icns "$iconset"

"$venv/bin/python" -m PyInstaller --noconfirm --clean --log-level WARN --windowed --copy-metadata pillow \
    --name MinusTwo --icon "$PWD/out/pyi-work-macos/MinusTwo.icns" \
    --osx-bundle-identifier io.github.gminustwo.minustwo --distpath out/pyi-dist-macos \
    --workpath out/pyi-work-macos --specpath out/pyi-work-macos --paths "$PWD/scripts" "${hidden[@]}" \
    "$PWD/launcher/minustwo_launcher.py"

# The package is assembled in a fresh staging folder and zipped from there.
dest=out/stage/minustwo-macos
rm -rf -- out/stage
mkdir -p "$dest"
cp -R out/pyi-dist-macos/MinusTwo.app "$dest/"
app=$dest/MinusTwo.app
res=$app/Contents/Resources
mkdir -p "$res/bin" "$res/dylibs" "$res/launcher"
# The executables without debug symbols (out/ keeps them for crash reports).
for exe in minustwo minustwo-gpu-capabilities; do
    strip -S -o "$res/bin/$exe" "out/$exe"
done
cp "$deps/lib/libvulkan_kosmickrisp.dylib" "$res/dylibs/"
# Every @rpath library they load, from the dependencies build_sh made (out/macos-deps/lib).
bundle() {
    local name
    for name in $(otool -L "$1" | awk 'NR > 1 && $1 ~ /^@rpath\// {sub("@rpath/", "", $1); print $1}'); do
        [[ -f $res/dylibs/$name ]] && continue
        [[ -f $deps/lib/$name ]] || { echo "Missing $deps/lib/$name (needed by $1)" >&2; exit 1; }
        cp -L "$deps/lib/$name" "$res/dylibs/$name"
        chmod u+w "$res/dylibs/$name"
        bundle "$res/dylibs/$name"
    done
}
for file in "$res/bin/minustwo" "$res/bin/minustwo-gpu-capabilities" "$res/dylibs/libvulkan_kosmickrisp.dylib"; do
    bundle "$file"
done
# Library paths inside the bundle only: the build's absolute rpaths (this repo) go, dylibs/ comes in.
relink() {
    local file=$1 rpath=$2 old
    for old in $(otool -l "$file" | awk '/cmd LC_RPATH/ {getline; getline; print $2}'); do
        install_name_tool -delete_rpath "$old" "$file" 2>/dev/null
    done
    install_name_tool -add_rpath "$rpath" "$file" 2>/dev/null
    codesign --force --sign - "$file" 2>/dev/null
}
relink "$res/bin/minustwo" @executable_path/../dylibs
relink "$res/bin/minustwo-gpu-capabilities" @executable_path/../dylibs
for lib in "$res"/dylibs/*.dylib; do
    relink "$lib" @loader_path
done
# The Vulkan loader finds KosmicKrisp through the manifest beside the game (src/mt_vulkan_driver.h).
sed 's#"library_path"[^,]*#"library_path": "../dylibs/libvulkan_kosmickrisp.dylib"#' \
    "$deps/share/vulkan/icd.d/kosmickrisp_mesa_icd.json" > "$res/bin/kosmickrisp_mesa_icd.json"
cp run.py LICENSE README.md "$res/"
cp -R scripts patches "$res/"
cp launcher/minustwo.png "$res/launcher/"
find "$res" -name __pycache__ -prune -exec rm -r {} +
plist=$app/Contents/Info.plist
plutil -replace CFBundleShortVersionString -string "$version" "$plist"
plutil -replace CFBundleVersion -string "$version" "$plist"
plutil -replace CFBundleDisplayName -string MinusTwo "$plist"
plutil -replace LSMinimumSystemVersion -string 26.0 "$plist"
plutil -replace NSHighResolutionCapable -bool true "$plist"
plutil -replace LSApplicationCategoryType -string public.app-category.games "$plist"
codesign --force --sign - "$app"

# Play MinusTwo.app: a script that runs MinusTwo --play (packaging/macos/play-minustwo).
play=$dest/Play\ MinusTwo.app
mkdir -p "$play/Contents/MacOS" "$play/Contents/Resources"
install -m755 packaging/macos/play-minustwo "$play/Contents/MacOS/play-minustwo"
cp out/pyi-work-macos/MinusTwo.icns "$play/Contents/Resources/"
cat > "$play/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>play-minustwo</string>
    <key>CFBundleIdentifier</key><string>io.github.gminustwo.minustwo.play</string>
    <key>CFBundleName</key><string>Play MinusTwo</string>
    <key>CFBundleDisplayName</key><string>Play MinusTwo</string>
    <key>CFBundleIconFile</key><string>MinusTwo</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$version</string>
    <key>CFBundleVersion</key><string>$version</string>
    <key>LSMinimumSystemVersion</key><string>26.0</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF
cp packaging/macos/README-macOS.txt "$dest/"

# No game files and nothing a player made (saves, shader caches, settings, logs) may ship.
forbidden=$(find "$dest" \( -iname eboot.bin -o -iname sce_module -o -iname sce_sys -o -iname savedata \
    -o -iname user -o -iname cache -o -iname '*.log' -o -iname minustwo.ini -o -iname settings.json \
    -o -iname mods.json -o -iname patches.json -o -iname fsr4_shaders \
    -o -iname '*.spv' -o -iname '*.pkg' -o -iname '*.sprx' -o -iname '*.prx' -o -iname '*.pac' \
    -o -iname '*.at9' -o -iname '*.mp4' -o -iname 'pic1.png' -o -iname 'icon0.png' \) -print)
if [[ -n $forbidden ]]; then
    echo "Package holds files that must not ship:" >&2
    echo "$forbidden" >&2
    exit 1
fi
# Nor the builder's home folder, whose path names the account (/Users/<name>): build paths are kept out
# of the binaries (gpu/CMakeLists.txt, packaging/macos/deps.sh).
if leaks=$(grep -rlF "$HOME/" "$dest"); then
    echo "Package holds paths in your home folder ($HOME):" >&2
    echo "$leaks" >&2
    exit 1
fi

mkdir -p dist
rm -f dist/minustwo-macos.zip
ditto -c -k --keepParent "$dest" dist/minustwo-macos.zip
(cd dist && shasum -a 256 minustwo-macos.zip > minustwo-macos.zip.sha256)
# dist/minustwo-macos: the apps to play from (player data is in Application Support, not here),
# refreshed only while nothing runs from it.
if pgrep -f "$PWD/dist/minustwo-macos/" >/dev/null; then
    echo "dist/minustwo-macos is in use: not refreshed; the zip is ready." >&2
else
    rm -rf dist/minustwo-macos
    cp -R "$dest" dist/minustwo-macos
fi
du -sh "$dest" dist/minustwo-macos.zip
