# Minus Zero

**Minus Zero** is an unofficial native PC port of **GODZILLA** (PlayStation 4, Bandai Namco / Natsume Atari, 2015)
for Windows 10 and 11. It is a personal project, for playing a copy of the game you own.

> **No game files are included.** You need a dump of your own PS4 copy: the folder with `eboot.bin`, `sce_module`
> and `sce_sys` (tested: CUSA02046, version 01.01). This project is not affiliated with or endorsed by Toho, Bandai
> Namco, Natsume Atari or Sony Interactive Entertainment. GODZILLA is a trademark of Toho Co., Ltd.

## How it works

The PS4 runs x86-64 code, so the game's own executable runs directly on the PC's processor: nothing is emulated on
the CPU side. A runtime written for this port replaces the PS4 system libraries the game calls (memory, threads, files,
the audio mixer, fonts, controller, saves, dialogs, PSN), and the PS4 graphics commands and shaders are translated to
Vulkan by the [shadPS4](https://github.com/shadps4-emu/shadPS4) video core. The approach and most of the
infrastructure come from [SpartanPort](https://github.com/FelipePrado0/SpartanPort), the God of War III port (see
[Credits](#credits)); what is GODZILLA's own is listed under *What this port adds*.

| Part | Where | What it does |
|---|---|---|
| Preparation | `scripts/` | Reads `eboot.bin`, applies its relocations and links the game's own `libc` and `libSceFios2` into one image |
| Loader | `src/probe.c` | Loads the image, binds its imports to the runtime and starts the game's original code |
| Runtime | `src/runtime_*.c` | The PS4 system functions: kernel, memory, threads, files, audio (AudioOut and the Ngs2 mixer), fonts, pad, saves, dialogs, services |
| GPU | `gpu/shadps4/` | The shadPS4 video core: PS4 GPU commands and shaders to Vulkan, with SpartanPort's background shader cache |
| In-game menu | `gpu/shim/` | Dear ImGui menu (Insert, or R3 + L2) with display, performance and overlay settings |
| Launcher | `launcher/minuszero_launcher.py` | Game folder, settings, controls and the Play button |

### What this port adds for GODZILLA

- **libSceNgs2**, the PS4's software audio mixer, which the game uses for all its music and sound effects: sampler
  voices decoding ATRAC9 (LibAtrac9) or PCM waveform blocks with loops and repeats, pitch, envelopes, port volumes,
  matrices and patches between voices, the custom submixer bus, mastering voices into the game's audio outputs with a
  peak limiter, and the RIFF waveform parser (`src/runtime_ngs2.c`).
- **libSceFont** on FreeType: the game renders its subtitles glyph by glyph from the FOT-NewCinemaB font it ships,
  and the PS4 system font set is stood in for by Windows' fonts (`src/runtime_font.c`).
- Kernel event flags, the offline PSN, matchmaking, Rudp, friend list, error dialog, broadcasting, screenshot and
  Share Play contracts the game needs to start, and message dialogs shown over the game for you to answer.
- The add-ons (the pre-order early unlocks and the diorama figure sets, whose content is in the base game) are
  reported as owned. Turn it off in the launcher, or with `MZ_UNLOCK_ADDONS=0`.
- Keyboard and mouse defaults for GODZILLA's controls, mouse camera.
- The God of War III specific parts of SpartanPort (cheats, QTE helpers, orb multipliers, button-icon and pause-menu
  patching) are removed.

## What works

Tested on Windows 11 with a 32-thread CPU and a Vulkan 1.3 GPU, from the first start to gameplay:

- The opening movie (H.264 with its audio), the title screen, the first-start dialogs and the save data creation.
- Gameplay in the 1954 prologue: rendering at a steady 60 FPS (p99 frame time 17.6 ms), HUD, radar, tutorials,
  the operator's portrait and subtitles.
- Music and sound effects through Ngs2, movie audio through AudioOut.
- Controller (any SDL3 gamepad as the PS4 pad) and keyboard with mouse.

### Known limits

- The game renders at 1920x1080. Rendering above it needs more than the game's own resolution switch: its scene
  targets and interface are sized for 1080p in several places (see *Notes for developers*).
- Ngs2 effects (reverb, filters, distortion, the mastering EQ) pass the sound unchanged; the reverb bus is silent.
- The PS4 system font (SST) is replaced by Windows' Segoe UI and Japanese fonts; set `MZ_SYSTEM_FONT` to other font
  files (separated by `;`) if you prefer.
- Online features (rankings, matchmaking) are offline: the game sees a console that is not signed in to PSN.
- Only the US release (CUSA02046 01.01) has been tested.

## Playing

1. Build the port (below) or use a packaged copy, and open **`MinusZero.exe`** (or run
   `python launcher/minuszero_launcher.py`).
2. On **Play**, click **Browse…** and choose your GODZILLA folder (the one with `eboot.bin`). The launcher checks it
   and shows the game's own cover picture from your dump.
3. Adjust **Settings** and **Controls** if you like, then **PLAY**. The first start prepares the game image; shaders
   are compiled the first time they are needed and cached in `user/`.

`Play MinusZero.exe` (or `MinusZero.exe --play`) starts the game with the saved settings without the launcher window,
for a desktop shortcut or Steam's *Add a Non-Steam Game*.

Saves, the shader cache and `user/last_run.log` are in the `user` folder next to the port; settings are in
`settings.json` (launcher) and `minuszero.ini` (display and input, also changed by the in-game menu).

### Controls

Any controller works as the PS4 pad. Keyboard and mouse defaults (change them on the launcher's **Controls** page):

| Key | PS4 button | In GODZILLA |
|---|---|---|
| W A S D | left stick | walk |
| Q / E | L1 / R1 | turn left / right |
| Mouse, arrow keys | right stick | camera |
| Left click | Square | normal attack |
| Right click | Triangle | strong attack |
| Space / Left Shift | Cross / Circle | |
| R / F | L2 / R2 | |
| Z / C | L3 / R3 | |
| Return | Options | pause |
| Tab, Backspace | touchpad | |
| I J K L | d-pad | |
| Insert | | the port's menu (R3 + L2 on a controller) |

## Building from source

Windows, with [MSYS2](https://www.msys2.org) and its **CLANG64** environment:

```bash
pacman -S --needed git patch \
  mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
  mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
  mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis,freetype} \
  mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross}
git submodule update --init --recursive
bash build.sh                       # out/minuszero.exe
bash packaging/windows/package.sh   # dist/minuszero-windows/ and its zip (needs a python.org Python)
```

Without the launcher, from a plain Windows Python:

```powershell
$env:MZ_GAME_DIR = 'D:\Games\CUSA02046'
python run.py
```

## Tests

```bash
python -m unittest discover -s tests -p "test_*.py"     # launcher, patch selection, preparation scripts
source ./msys2-env.sh                                     # in MSYS2 CLANG64, after build.sh:
ninja -C out/gpu ngs2-test eventflag-test services-test pad-map-test graphics-settings-test
out/gpu/ngs2-test.exe && out/gpu/eventflag-test.exe && out/gpu/services-test.exe
```

## Tools for testing

- `python tools/autoplay.py --game <folder> --seconds 200 --press 125:cross --hold 160-190:ly=0` plays the game
  unattended: scripted pad input, answered dialogs, a screen dump every few seconds (`out/autoplay/sheet.png`) and the
  loudness of every audio output.
- `MZ_NGS2_TRACE=1` logs the game's racks, voices, parameters and mixer levels; `MZ_FONT_TRACE=1` its font calls;
  `MZ_FRAME_STATS=1` frame rate and pacing; `MZ_CAPTURE_TRIGGER=<file>` records every pass of the next frame.
- `out/minuszero.exe out/boot-linked.bin --app0 <folder> --check-imports` lists the system functions the game imports
  that the runtime does not provide (none for CUSA02046 01.01).
- `python tools/gen_import_names.py out/link.json --aerolib <shadPS4>/src/core/aerolib/aerolib.inl` names the imports
  of another build in `src/import_names.inc`.

### Notes for developers

The game chooses its display size from `sceVideoOutGetResolutionStatus` (a five-entry table from 1280x720 to
1920x1080 at `0x4ee6f0`, clamped to 1080p at `0x175be1`); lifting that clamp and reporting a larger display
(`MZ_INTERNAL_WIDTH`/`MZ_INTERNAL_HEIGHT`) gives larger display buffers, but the scene targets and the interface stay
1920x1080, drawn in a corner. A resolution option needs those sizes found as well.

## Credits

- **SpartanPort** by Felipe Prado — the God of War III port this one is built from: the loader, the runtime, the
  launcher's design and the packaging. It is itself a fork of **Supermedo**'s Windows port of **deadinside28**'s
  [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc).
- **shadPS4 team** — the [shadPS4](https://github.com/shadps4-emu/shadPS4) video core that translates the GPU work,
  and the reference for the PS4 libraries' behaviour (NID tables, sceFont and AppContent conventions).
- **cuesta4** — the shadPS4 rendering fixes SpartanPort carries.
- **P.T. for PC** (Hallway Engine) — the other port this project looked at for its approach and presentation.
- [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9), [FreeType](https://freetype.org),
  [FFmpeg](https://ffmpeg.org), [SDL3](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui),
  [sirit](https://github.com/shadps4-emu/sirit), [magic_enum](https://github.com/Neargye/magic_enum),
  [miniz](https://github.com/richgel999/miniz), [xbyak](https://github.com/herumi/xbyak),
  [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
  [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan), [MSYS2](https://www.msys2.org), [LLVM](https://llvm.org) and
  [Pillow](https://python-pillow.org).

## License

GNU GPL v2 or later ([LICENSE](LICENSE)), as SpartanPort. Third-party components keep their own licenses.

## AI disclosure

This port was made with an AI coding assistant (Claude), working from SpartanPort's source, shadPS4's library
references and the game's own behaviour observed in test runs.
