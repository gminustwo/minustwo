Minus Two - GODZILLA (PS4) for Linux and the Steam Deck
=======================================================

No game files are included. You need your own copy of GODZILLA (CUSA02046) as a folder
containing eboot.bin, sce_module and sce_sys. Tested: CUSA02046 version 01.01.

Requirements
- A 64-bit Linux with glibc 2.39 or newer: SteamOS 3, Ubuntu 24.04+, Debian 13, Fedora, Bazzite.
- A 64-bit x86 processor with AVX (the Steam Deck has it).
- A graphics card with Vulkan 1.3 (the Steam Deck has it; on a desktop, Mesa's RADV or a
  recent NVIDIA driver).

Steam Deck
Confirmed: GODZILLA runs well on the Steam Deck at 30 FPS, rendering at 1080p. For steady
pacing, set the frame rate limit to 30 on the launcher's Display & FPS page.

1. Switch to Desktop Mode (Steam button > Power > Switch to Desktop).
2. Copy your GODZILLA folder to the Deck or a microSD card, for example ~/Games/CUSA02046.
3. Download minustwo-x86_64.AppImage, for example to ~/Games. In the file manager, right
   click it > Properties > Permissions > tick "Is executable" (or: chmod +x minustwo-x86_64.AppImage).
4. Double-click it. In the launcher, choose the GODZILLA folder on the Play page and click PLAY
   once: the first start prepares the game image and takes a little longer.
5. For Game Mode: open Steam, Games > Add a Non-Steam Game to My Library > Browse, pick the
   AppImage (set the file type filter to "All Files"). Then in its Properties, add
   --play
   to Launch Options. Steam then starts the game straight away with your saved settings.
   Use the default controller layout (Gamepad); in the game, R3 + L2 opens the port's menu.

Other Linux PCs
- chmod +x minustwo-x86_64.AppImage, then run it. The launcher works as on Windows.
- "minustwo-x86_64.AppImage --play" starts the game without the launcher.

Online play
- VS Mode online is not supported yet; playing it between two copies of the port is being
  looked at.

Updates
- When a new version is out, the launcher shows it at the bottom left; "Update" replaces the
  AppImage with it and opens the launcher again (saves, settings and mods are kept).

Files
- Saves, settings, mods, the prepared game and the shader cache are in ~/.local/share/minustwo
  (the AppImage itself is read-only). Keep that folder when you update.
- If the game does not start, open Advanced > Show the last game log in the launcher.

Questions, bug reports and news: the project's Discord server, https://discord.gg/VVthdPE4q
