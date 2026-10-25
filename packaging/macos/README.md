# minustwo on macOS

Experimental. The port builds and starts on macOS 26 or newer on Apple silicon; it does not yet
reach the opening movie (see *Status*).

The game's code is x86-64 and runs directly on the CPU, so on a Mac `minustwo` is an x86-64 program
that runs under **Rosetta 2**, with every library it loads built for x86-64 as well. The GPU work goes
to Metal through **KosmicKrisp**, LunarG's Mesa Vulkan driver, as shadPS4 uses it on macOS: the
renderer needs sparse buffers, `robustBufferAccess2` and null descriptors, which MoltenVK does not
have.

## Build

```bash
softwareupdate --install-rosetta --agree-to-license   # once
xcode-select --install                                # or Xcode
brew install cmake ninja pkgconf nasm glslang llvm spirv-tools spirv-llvm-translator
git submodule update --init --recursive
bash build.sh        # out/minustwo, out/minustwo-gpu-capabilities
```

The first build runs `packaging/macos/deps.sh`, which builds the x86-64 libraries from source into
`out/macos-deps` (SDL3, FFmpeg, FreeType, the Vulkan loader, fmt, Zydis, xxHash, VMA, robin-map, Boost
headers) and KosmicKrisp from the commit shadPS4 b4e7ae7 pins (Mesa's compiler tools for this Mac with
Homebrew's LLVM, then the x86-64 driver). It takes a few minutes once; each library has a stamp in
`out/macos-deps/stamps` and is rebuilt only when its version in the script changes.

## Run

```bash
python3 launcher/minustwo_launcher.py               # the launcher (python.org Python with Tk, Pillow)
MT_GAME_DIR=~/Games/CUSA02046 python3 run.py        # or the game directly
```

`run.py` sets `ROSETTA_ADVERTISE_AVX=1`: Rosetta runs AVX, F16C, BMI and MOVBE code but reports them in
CPUID only when asked, and `minustwo` starts itself again with it when started another way. In the
game, **Command-comma** opens the port's menu (Mac keyboards have no Insert key).

## Package

```bash
bash packaging/macos/package.sh   # dist/minustwo-macos/ (MinusTwo.app, Play MinusTwo.app) and its .zip
```

Run it after `build.sh`. `MinusTwo.app` is the launcher, frozen with PyInstaller from a python.org Python
with Tk, so players need no Python. Its `Contents/Resources` holds the rest of the port:
- `run.py`, the scripts and the patches;
- `bin/minustwo` with the KosmicKrisp manifest beside it;
- `dylibs/` with the libraries from `out/macos-deps`, relinked to `@executable_path/../dylibs`.

When frozen on macOS, the launcher takes `Contents/Resources` as the port folder (`port_folder()`) and
keeps saves, settings and the prepared game in `~/Library/Application Support/MinusTwo`.
`Play MinusTwo.app` runs `MinusTwo --play` from the `MinusTwo.app` beside it or in Applications, like
`Play MinusTwo.exe` does, and shows an alert when the game cannot start.

Both apps are signed ad hoc. They open on the Mac that built them; a downloaded copy needs
right-click > Open (there is no Developer ID or notarization). `README-macOS.txt` is the player's guide.

## How it differs from Linux and Windows

- **Guest addresses.** Apple silicon keeps `0xfc0000000`-`0x7000000000` of every process for the
  commpage and the GPU's shared address space, so the PS4 user range starts at `0x7000000000` (still
  below 1 TiB, as shadPS4 does on macOS). Both guest ranges are reserved at start, since macOS maps
  from the bottom of the address space up (`src/runtime_memory.c`). Guest memory is an unlinked POSIX
  shared memory object instead of a memfd.
- **Guest TLS.** `gs` points at the thread's pthread slots; the TCB lives in a pthread key and the
  loader rewrites `mov rax, gs:[0]` to that slot, as on Windows (`src/runtime_thread.c`).
- **Threads.** Windows and their events belong to the main thread on macOS: the window loop runs
  there and the game starts on a thread of its own (`mtgpu_window_loop`).
- **Signals.** A write to a protected page is `SIGBUS`; register access goes through the macOS
  thread state (Rosetta presents the x86-64 one).
- The Linux-only calls (`memfd`, `arch_prctl`, `/proc`, `clock_nanosleep`, timed locks, ...) have
  counterparts in `src/compat_mac.{h,c}`; `gpu/shim/mt_mac_compat.h` provides `gettid`.

## Status

Plays, on an M3 Max under Rosetta 2. The tests pass: the CPU feature check, display and gamepad
enumeration, the Vulkan smoke test, the loader tests and the runtime's C tests. The game boots and plays
the opening movie with sound. The title screen and its menu work, and the tutorial stage loads and is
controllable, with sound effects and voices through Ngs2 and CoreAudio.

Two things are different on this driver:

- **No guest multisampling.** KosmicKrisp resolves the game's 4-sample targets to one flat colour (the
  movie showed as a single colour that changed). The game's multisampled images are therefore
  single-sampled by default, as with AMD's and Intel's Windows drivers (`mt_msaa_default.h`);
  `MT_NO_MSAA=0` turns multisampling back on for testing.
- **One join per HLE thread.** Closing a movie joins its decoder threads from two threads at once.
  glibc fails the second join, and the port already handled that; macOS blocked it forever, so the game
  hung on a black screen after a movie. `Libraries::Kernel::Thread` now lets one thread join and makes
  the others wait for it.

Known gap:

- The game files must be a good copy. A damaged repack (its `.pac` LZS blobs full of distance-0
  matches) stops the game while loading `system.pac`, with "Data member ... m_unsacnceListCount",
  "Resolving fixup for 'PUInt32' returned NULL pointer" or "A heap error is detected".
