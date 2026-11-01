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
   download) to add the 52 fighters cut from the game to the roster.
5. Pick your settings and press Play.

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
  Backend: Vulkan is the default and the better tested. Direct3D 12 works but has had less play time
  at 2x and above. If something looks wrong on one, try the other.

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
  - Career mode: opponents' shorts logos and tattoos can look pixelated at higher render scales.
  - Created-fighter portraits saved by older versions stay garbled; make the portrait again.
  - Bloom off in Settings currently has no visible effect.

CONTROLS
An Xbox controller is recommended. F4 in game: quick settings and Quit.
DLSS 5 (optional, needs an NVIDIA RTX card) is set up in the launcher's DLSS 5 tab.

NO WARRANTY. Not affiliated with or endorsed by THQ, Yuke's, Zuffa or the UFC.
