# UFC Undisputed 3 Recomp

A native Windows PC port of **UFC Undisputed 3** (Xbox 360), made by static recompilation with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk). It runs the original game code natively. It is not an emulator.

**This download contains no game files.** You need your own legally obtained copy of UFC Undisputed 3
for Xbox 360 as a disc image (`.iso`). DLC is optional and also comes from your own copy.

`ufc3.exe` is the game's own code translated to native PC code, which is how a recompilation works.

## Features

- Runs natively at any resolution (render scale up to 4K-class and beyond)
- Vulkan (default) or Direct3D 12
- **F4 in game:** quick settings and Quit
- Bloom off and depth-of-field blur off (optional)
- 60 fps cutscenes, walkouts and replays (optional)
- First-run prompts are saved properly (no repeat grappling/tutorial questions)
- Working DLC from your own packages
- **Mods folder:** files laid over the game without touching your game files
- Roster beyond the game's 600-fighter table (up to 1024), for roster mods
- Optional cheats: unlock everything, free unlock points (off by default)
- Optional per-fighter walkout music from your own audio files
- Optional DLSS 5 add-on (NVIDIA RTX)
- Alt-tab and focus switches are instant (no black screen from exclusive fullscreen)

## Install

1. Download `UFC3-Recomp-<version>.zip` from [Releases](../../releases) and unzip anywhere, for example
   `C:\Games\UFC3 Recomp\` (not Program Files).
2. Run **UFC3 Launcher.exe**.
3. **Setup** tab: choose your UFC 3 `.iso`. The launcher checks it and copies the game files into `assets\` (about 6.5 GB).
   Optionally point it at your DLC packages.
4. Pick your settings in the **Settings** tab, then press **Play**.

The launcher checks for new releases and can update itself. Your saves, settings and mods are never touched by updates.

## Mod packs

Mod packs are zips with a `modpack.json` and a `mods\` folder. Install them from the launcher's **Mod Pack** tab;
uninstall removes exactly the files the pack added. Mod packs are separate downloads and are not part of this repository.

## Controls

An Xbox controller is recommended. Press **F4** at any time for quick settings and Quit.

## Building from source

See [BUILDING.md](BUILDING.md).

## Credits and licences

See [CREDITS.md](CREDITS.md) and `licenses\`.

Not affiliated with or endorsed by THQ, Yuke's, Zuffa or the UFC. No game code, assets or DLC are distributed
in this repository. Please support official releases.
