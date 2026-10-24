# Minus Two

**Minus Two** is an unofficial native PC port of **GODZILLA** (PlayStation 4, Bandai Namco / Natsume Atari, 2015)
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
| Launcher | `launcher/minustwo_launcher.py` | Game folder, settings, controls and the Play button |

### What this port adds for GODZILLA

- **libSceNgs2**, the PS4's software audio mixer, which the game uses for all its music, voices and sound effects:
  sampler voices decoding ATRAC9 (LibAtrac9) or PCM waveform blocks with loops and repeats, streams (the voice lines,
  read from disk into a ring buffer the game refills while they play), pitch, envelopes, port volumes, matrices and
  patches between voices, the custom submixer bus, mastering voices into the game's audio outputs with a peak
  limiter, and the RIFF waveform parser (`src/runtime_ngs2.c`).
- **Rendering above 1080p**: the in-game menu's *Render resolution* (1440p, 1800p, 4K, 8K, ...) draws the whole frame,
  scene and interface, at that size on the GPU side: the game's 1080p targets get larger stand-ins, its 4x MSAA
  interface pass included (`gpu/shadps4/video_core/renderer_vulkan/vk_scene_resolution.cpp`). A size of another shape
  keeps the game's 16:9 picture at that height.
- **Movies without stutter**: the PS4 movie player's clock followed the game's audio pulls, so frames were handed out
  one, then three display refreshes apart (shadPS4 shows the same). It now runs on a steady clock locked to the
  game's 60 Hz frame requests and waits for the sound only when the sound falls behind. Each decoded frame is
  copied into the game's frame buffers in turn when it is handed out: the game still draws the previous one for a
  refresh, and the decoder used to overwrite it with a later frame (the picture went back and forth).
- **Even frame pacing**: *Match refresh rate* (on by default) runs a display whose refresh rate is not a multiple of
  60 (144 Hz) at 120 Hz in Borderless and Fullscreen, and the emulated 60 Hz vblank ticks on every 2nd vblank of the
  display itself (`gpu/shim/mt_vblank.cpp`, D3DKMT), so every frame lasts exactly two refreshes. With VRR, or a
  display whose vblanks do not come steadily, it stays on its timer.
- **libSceFont** on FreeType: the game renders its subtitles glyph by glyph from the FOT-NewCinemaB font it ships,
  and the PS4 system font set is stood in for by Windows' fonts (`src/runtime_font.c`).
- Kernel event flags, the offline PSN, matchmaking, Rudp, friend list, error dialog, broadcasting, screenshot and
  Share Play contracts the game needs to start, and message dialogs shown over the game for you to answer.
- The add-ons (the pre-order early unlocks and the diorama figure sets, whose content is in the base game) are
  reported as owned. Turn it off in the launcher, or with `MT_UNLOCK_ADDONS=0`.
- Keyboard and mouse defaults for GODZILLA's controls, mouse camera.
- The God of War III specific parts of SpartanPort (cheats, QTE helpers, orb multipliers, button-icon and pause-menu
  patching) are removed.

## What works

Tested on Windows 11 with a 32-thread CPU and a Vulkan 1.3 GPU, from the first start to gameplay:

- The opening movie (H.264 with its audio), the title screen, the first-start dialogs and the save data creation.
- Gameplay in the 1954 prologue: rendering at a steady 60 FPS (p99 frame time 17.6 ms), HUD, radar, tutorials,
  the operator's portrait and subtitles.
- Music, voices and sound effects through Ngs2, movie audio through AudioOut.
- 4K rendering (Render resolution 4K) at 60 FPS on an RTX 5090.
- Controller (any SDL3 gamepad as the PS4 pad) and keyboard with mouse.

### Known limits

- Effects at a quarter of 1080p and below (the bloom chain) keep their size above 1080p; they are blurs, so it hardly
  shows.
- On a fixed-refresh display that is not a multiple of 60 Hz (144 Hz), 60 FPS cannot be shown evenly; G-Sync /
  FreeSync or a 60 or 120 Hz desktop mode gives even motion.
- Ngs2 effects (the custom submixer's effect modules, filters, distortion, the mastering EQ) pass the sound unchanged;
  only the volumes, routing and a limiter are applied.
- The PS4 system font (SST) is replaced by Windows' Segoe UI and Japanese fonts; set `MT_SYSTEM_FONT` to other font
  files (separated by `;`) if you prefer.
- Online features (rankings, matchmaking) are offline: the game sees a console that is not signed in to PSN.
- Only the US release (CUSA02046 01.01) has been tested.

## Playing

1. Build the port (below) or use a packaged copy, and open **`MinusTwo.exe`** (or run
   `python launcher/minustwo_launcher.py`).
2. On **Play**, click **Browse…** and choose your GODZILLA folder (the one with `eboot.bin`). The launcher checks it
   and shows the game's own cover picture from your dump.
3. Adjust **Settings** and **Controls** if you like, then **PLAY**. The first start prepares the game image; shaders
   are compiled the first time they are needed and cached in `user/`.

`Play MinusTwo.exe` (or `MinusTwo.exe --play`) starts the game with the saved settings without the launcher window,
for a desktop shortcut or Steam's *Add a Non-Steam Game*.

Saves, the shader cache and `user/last_run.log` are in the `user` folder next to the port; settings are in
`settings.json` (launcher) and `minustwo.ini` (display and input, also changed by the in-game menu).

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
bash build.sh                       # out/minustwo.exe
bash packaging/windows/package.sh   # dist/minustwo-windows/ and its zip (needs a python.org Python)
```

Without the launcher, from a plain Windows Python:

```powershell
$env:MT_GAME_DIR = 'D:\Games\CUSA02046'
python run.py
```

### Releases and updates

The launcher checks the latest release of the GitHub repository in `GITHUB_REPO`
(`launcher/minustwo_launcher.py`, `owner/name`; the repository must be public, empty turns the check off) and
offers it when its tag is a higher version than `VERSION` in the same file. To publish a version:

1. Raise `VERSION` (for example to `0.2.0`) and commit.
2. `bash build.sh` and `bash packaging/windows/package.sh`: `dist/minustwo-windows.zip` and
   `dist/minustwo-windows.zip.sha256`.
3. On GitHub, create a release tagged `v0.2.0` and attach both files. The launcher refuses a zip without the
   `.sha256` beside it, or one that does not match it.

"Update" downloads the zip, checks it, and the new `MinusTwo.exe` (`--install-update`) copies itself over the
installation once the launcher has closed, never the player's files (`USER_FILES`: `user`, settings, mods), then
opens it.

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
- `MT_NGS2_TRACE=1` logs the game's racks, voices, parameters and mixer levels; `MT_AUDIO_TRACE=1` the sound files it
  opens; `MT_AVPLAYER_TRACE=1` (2: also every request) when the movie player hands out frames; `MT_FONT_TRACE=1` its
  font calls; `MT_FRAME_STATS=1` frame rate and pacing; `MT_CAPTURE_TRIGGER=<file>` records every pass of the next
  frame; `MT_SCENE_DEBUG=<file>` (created while the game runs) prints which passes of one frame are drawn above
  1080p and why the others are not; `MT_SCENE_SIZE=<width>x<height>` overrides the render resolution.
- `MT_PRESENT_TRACE=<file>` samples every frame the game shows into `<file>` and logs its flip, its present and every
  display vblank; `python tools/present_report.py <run.log> <file> <movie.mp4>` matches those frames to the movie's
  own and tells how long each movie frame stayed on screen (with `MT_AVPLAYER_TRACE=1`, only while the movie plays).
  `MT_VBLANK_LOCK=0` keeps the emulated vblank on its timer instead of the display's vblanks.
- `out/minustwo.exe out/boot-linked.bin --app0 <folder> --check-imports` lists the system functions the game imports
  that the runtime does not provide (none for CUSA02046 01.01).
- `python tools/gen_import_names.py out/link.json --aerolib <shadPS4>/src/core/aerolib/aerolib.inl` names the imports
  of another build in `src/import_names.inc`.

### Notes for developers

The game chooses its display size from `sceVideoOutGetResolutionStatus` (a five-entry table from 1280x720 to
1920x1080 at `0x4ee6f0`, clamped to 1080p at `0x175be1`); lifting that clamp gives larger display buffers, but the
scene targets and the interface stay 1920x1080, drawn in a corner. So the render resolution works on the GPU side
instead: every 1920x1080 (and 960x540) render target gets a larger stand-in image, shaders see the guest's pixel
coordinates (`push_data.scene_size`), and the frame's last pass, which resolves the 4x MSAA interface target with
texel fetches at its own pixel, reads that target's multisampled stand-in at the stand-in's own coordinates
(`Rasterizer::host_fetch`).

The game's sampler rack is created without options and the game uses as many voices as `sceNgs2RackGetInfo`
reports; the runtime gives such a rack 256. Voice lines are streams: play and pause before the data, the first part
of a 16 KiB ring buffer (blocks flag 1), then each refilled part with its own address (flag 3); ATRAC9 superframes
that the ring's end splits are joined.

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
