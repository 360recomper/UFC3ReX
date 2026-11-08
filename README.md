# UFC Undisputed 3 Recomp

A native Windows PC port of **UFC Undisputed 3** (Xbox 360), made by static recompilation with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk). It runs the original game code natively. It is not an emulator.

**This download contains no game files.** You need your own legally obtained copy of UFC Undisputed 3
for Xbox 360 as a disc image (`.iso`). DLC is optional and also comes from your own copy.

`ufc3.exe` is the game's own code translated to native PC code, which is how a recompilation works.

## Features

- Runs natively at any resolution (render scale up to 4K-class and beyond)
- Native Direct3D 12 renderer (default): the game draws directly through Direct3D 12 instead of an emulated GPU.
  Optional AMD FSR 1 upscaling. The emulated renderer (Vulkan or Direct3D 12) stays as the fallback.
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
- Blood stays where it lands on the mat at higher render scales
- Career mode opponents and referees render properly (no black or invisible fighters)
- Career mode playable start to finish: no crash at the end of the first fight or at the first UFC main-card fight
- Created fighters: correct skin, portraits, magazine covers and walkout shirts, at any render scale
- Career mode: opponents' shorts logos and tattoos stay sharp at higher render scales

With the mod pack installed (separate download, see below), the same exe also turns on:

- **Blood and Guts:** bare-knuckle fights (right stick click, R3, on SELECT FIGHTER switches gloves and bare fists,
  UFC and PRIDE) with heavy blood that builds over the fight; gloved fights stay close to the original
  (amounts in the launcher's Settings > Gameplay)
- **Attire picker:** left stick click (L3) on SELECT FIGHTER cycles through more than two attires per fighter

Without the mod pack the game plays like the original: no Blood and Guts, no bare knuckle, no attire picker.

## Install

1. Download `UFC3-Recomp-<version>.zip` from [Releases](../../releases) and unzip anywhere, for example
   `C:\Games\UFC3 Recomp\` (not Program Files).
2. Run **UFC3 Launcher.exe**.
3. **Setup** tab: choose your UFC 3 `.iso`. The launcher checks it and copies the game files into `assets\` (about 6.5 GB).
   Optionally point it at your DLC packages.
4. Pick your settings in the **Settings** tab, then press **Play**. The first Play builds the native renderer's
   shader cache from your own game files (only once).

The launcher checks for new releases and can update itself. Your saves, settings and mods are never touched by updates.

## Mod packs

Mod packs are zips with a `modpack.json` and a `mods\` folder. Install them from the launcher's **Mod Pack** tab;
uninstall removes exactly the files the pack added. Mod packs are separate downloads and are not part of this repository.

**Updating a mod pack:** the launcher doesn't download mod packs for you. When a new version comes out, download the new zip and
install it from the **Mod Pack** tab. The launcher removes the old version first, then installs the new one. Your saves and created
fighters are kept. Update the game (launcher) first if the pack's notes ask for a newer version.

## Controls

An Xbox controller is recommended. Press **F4** at any time for quick settings and Quit.
With the mod pack installed, on SELECT FIGHTER: click the right stick (**R3**) to switch between gloves and bare fists,
and the left stick (**L3**) for the next attire.

## Building from source

See [BUILDING.md](BUILDING.md).

## Credits and licences

This project is licensed under the GNU General Public License v3.0 ([LICENSE](LICENSE)). The native renderer comes
from the SmackDown vs. Raw 2010 recomp by nefariousjosiah (GPL-3.0), which builds on re:Blue's renderer core
(BSD 3-Clause), plume and XenosRecomp (MIT). These and the SDK's libraries keep their own licences: see
[CREDITS.md](CREDITS.md) and `licenses\`.

Not affiliated with or endorsed by THQ, Yuke's, Zuffa or the UFC. No game code, assets or DLC are distributed
in this repository. Please support official releases.
