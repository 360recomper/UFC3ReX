UFC Undisputed 3 Recomp
=======================

A native Windows PC port of UFC Undisputed 3 (Xbox 360), made by static
recompilation. It runs the original game's code natively. It is not an emulator.

NOTE: ufc3.exe is the game's own code translated to native code (that is how a
recompilation works), not an emulator and not new code written from scratch.

THIS DOWNLOAD CONTAINS NO GAME FILES. You need your own UFC Undisputed 3 for
Xbox 360 as a disc image (.iso). Nothing is included from the game.

INSTALL
1. Unzip anywhere, for example C:\Games\UFC3 Recomp\  (not Program Files).
2. Run "UFC3 Launcher.exe".
3. Setup tab: choose your UFC 3 .iso. The launcher extracts the game files
   into the assets\ folder next to it (about 6.5 GB, takes a few minutes).
4. Optional: Mod Pack tab > install "UFC 3 Legends: Cut Fighters" (a separate
   download). It adds 58 fighters (the ones cut from the game plus "second
   version" legends), extra attires, the PRIDE Bushido ring, and it unlocks
   Blood and Guts, bare knuckle and the attire picker. Without it the game
   plays like the original.
5. Pick your settings and press Play. The first Play builds the native
   renderer's shader cache from your game files (only once).

FOLDERS (all next to ufc3.exe)
  assets\    the extracted game files (made by Setup)
  mods\      replacement files, laid over the game without changing it
  userdata\  your saves, created on first run
  ufc3.toml  settings (F4 in game edits these)

GRAPHICS
  Render scale (launcher Settings tab): 1x = 720p, 2x = 1440p, 3x = 4K, 4x = 5K+. It is how sharp
  the picture is drawn, and how hard your GPU works.
  Window / output size is separate: the game window, or your screen in borderless fullscreen (F11).
  The picture is scaled to fit it, so a higher render scale on a smaller screen still looks sharper.
  Renderer: Native (default) draws the game directly with Direct3D 12. Emulated is the fallback: if
  the native renderer can't start, the game runs on Emulated. Graphics backend (Vulkan / Direct3D 12)
  applies to Emulated only. DLSS 5 runs use Emulated.
  Upscaling (native only): Bilinear by default. AMD FSR 1 is optional: it sharpens a picture smaller
  than the window (for example 1x on a 1080p or bigger screen); a picture as big as the window is
  not changed.

WHAT'S NEW IN 1.1.3
  - Fix: menus drawn as plain white boxes on some PCs (seen on Windows setups whose Direct3D 12 can't create
    texture sizes that aren't a multiple of 4). Those menu textures are now converted on load, so menus look normal.

WHAT'S NEW IN 1.1.2
  - Fix: with the mod pack, L3 on SELECT FIGHTER now cycles attires out of the box (the attire
    picker setting was off by default, so it only worked if you turned it on yourself).

WHAT'S NEW IN 1.1.1
  - Fix: updating from 1.0.7 with the launcher's update button now gets the native renderer's
    pipeline seed (it is built into ufc3.exe), so there are no extra first-use stutters.

WHAT'S NEW IN 1.1.0
  - Native renderer, on by default: the game draws through its own Direct3D 12 renderer instead of
    the emulated GPU path. Optional FSR 1 upscaling, and a pipeline seed for fewer first-use stutters.
  - Blood and Guts (with the mod pack): heavy blood in bare-knuckle fights, about 1.5x the original
    with gloves, cuts, bloody knuckles, blood on the mat.
  - Attire picker (with the mod pack): press L3 on SELECT FIGHTER to cycle more than two attires.
    Old-game looks for disc fighters and recolours for added fighters.
  - New "second version" fighters (mod pack): Classic Tito Ortiz, Classic Chuck Liddell, Prime Matt
    Hughes, Prime Forrest Griffin, Prime Rampage Jackson, UFC-era Mark Coleman, Rookie Jon Jones.
  - New PRIDE venue (mod pack): the PRIDE Bushido ring. The venue screen shows its own name, logo
    and picture, and the PRIDE venue list scrolls.

FIRST IN 1.0.8 (supporter early access; needs the mod pack from 1.1.0 on)
  - Bare-knuckle fights: on SELECT FIGHTER, click the right stick (R3) to switch both fighters
    between gloves and bare fists. The fight is played the way you leave it. Works in UFC and
    PRIDE rules. Click R3 again to put the gloves back.
  - Heavy blood in bare knuckle: many cuts, blood that builds over the fight and runs down the
    chest, blood on your knuckles. Gloved fights stay close to the original. Set the amounts in
    the launcher: Settings, Gameplay.
  - Replays show the blood, not just the cuts.
  - Career mode: opponents' shorts logos and tattoos are sharp at higher render scales.

WHAT'S NEW IN 1.0.7
  - Added fighters are their real size in the cage (Tim Sylvia towers over Cain, Shaq is huge),
    and the tale of the tape shows their own height, age, weight, reach and flag.
  - UFC 2009 fighters' skin tone fixed (no more orange), no holes at the shirt armpits, gloves fixed.
  - PRIDE ring: added fighters no longer look washed out under the lights.
  - Steady skin lighting (on by default, launcher Settings): no dark tan look when a clinch or
    takedown starts. Untick it for the original game's look.
  - Goldberg can call added fighters by name during their walkouts (Jardine, Arlovski, Sylvia...).
  - Fixed a crash at startup when the game folder has non-English letters in its name.

KNOWN ISSUES
  - Select-screen pictures of fighters from UFC 2009/2010 are framed closer than the rest.
  - The created-fighter card portrait is a little low-res.
  - 8 added fighters skip the hometown line in their intro.
  - Goldberg names only one fighter in the walkouts, and not every fight.
  - Created-fighter portraits saved by older versions stay garbled; make the portrait again.
  - Bloom off in Settings currently has no visible effect.

CONTROLS
An Xbox controller is recommended. F4 in game: quick settings and Quit.
With the mod pack installed, on SELECT FIGHTER: right stick click (R3) = gloves or bare fists,
left stick click (L3) = next attire.
DLSS 5 (optional, needs an NVIDIA RTX card) is set up in the launcher's DLSS 5 tab.

LICENCE
  GNU GPL v3.0 (LICENSE.txt). Parts come from other projects under their own licences (the native
  renderer from the SmackDown vs. Raw 2010 recomp by nefariousjosiah, re:Blue, plume,
  XenosRecomp, the ReXGlue SDK and its libraries): see CREDITS.md.

NO WARRANTY. Not affiliated with or endorsed by THQ, Yuke's, Zuffa or the UFC.
