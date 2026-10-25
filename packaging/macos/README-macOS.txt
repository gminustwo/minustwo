Minus Two - GODZILLA (PS4) for macOS
====================================

No game files are included. You need your own copy of GODZILLA (CUSA02046) as a folder
containing eboot.bin, sce_module and sce_sys. Tested: CUSA02046 version 01.01.

Requirements
- A Mac with Apple silicon (M1 or newer) and macOS 26 or newer.
- Rosetta 2: the game's code is x86-64. If it is missing, install it in Terminal with
  softwareupdate --install-rosetta --agree-to-license

Starting
1. Put MinusTwo.app and Play MinusTwo.app together, for example in Applications.
2. Open MinusTwo. On the Play page, choose the GODZILLA folder and click PLAY. The first start
   prepares the game image and takes a little longer.
3. Play MinusTwo starts the game straight away with the saved settings, without the launcher
   window (like "MinusTwo.app/Contents/MacOS/MinusTwo --play").
   The apps are not signed by a developer: if macOS refuses to open one that was downloaded,
   right-click it > Open, or in Terminal: xattr -dr com.apple.quarantine MinusTwo.app

In the game
- Command-comma (or R3 + L2) opens the port's menu.
- The mouse turns the camera while the game window is active; Command-Tab releases it.
- Keyboard: WASD walk, Q/E turn, Space Cross, Left Shift Circle, Enter Options, Tab or Backspace
  the touch pad (it skips cutscenes). A game controller (Xbox, PlayStation, ...) works too.

Files
- Saves, settings, mods, the prepared game and the shader cache are in
  ~/Library/Application Support/MinusTwo (the apps themselves are not written into).
- If the game does not start, open Advanced > Show the last game log in the launcher.
