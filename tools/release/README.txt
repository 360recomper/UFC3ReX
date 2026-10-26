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
  Backend: Vulkan is the default and the better tested. Direct3D 12 works too. If something looks wrong on one, try the other.

WHAT'S NEW IN 1.0.6
  - Render scale 2x and above: created fighters (skin, face, portraits, shirts) no longer turn black
    or to colored noise, and damage and blood on the face and mat show up.
  - The black screen when a fighter is rocked is fixed (Vulkan).
  - Career mode: no crash at the end of the first real fight, and created-fighter pictures are right.
  - Fewer hitches when saving or applying clothes in the fighter creator.
  - Guard against a crash in Liddell vs Quarry (PRIDE) after loading the match several times.
  - Without the mod pack, some disc fighters (for example Brock Lesnar) could vanish from fighter select. Fixed.
  - Mod pack 1.0.6: no shin line on UFC 2009 fighters, walkout shirts fit, UFC 2010 fighters wear their own
    shirt designs, changed ratings apply to existing saves.

KNOWN ISSUES
  - Select-screen pictures of fighters from UFC 2009/2010 are framed closer than the rest.
  - The created-fighter card portrait is a little low-res.
  - At 3x, opponent sponsor logos are drawn at 1x.
  - 8 added fighters skip the hometown line in their intro.
  - Created-fighter portraits saved by older versions stay garbled; make the portrait again.
  - Bloom off in Settings currently has no visible effect.

CONTROLS
An Xbox controller is recommended. F4 in game: quick settings and Quit.
DLSS 5 (optional, needs an NVIDIA RTX card) is set up in the launcher's DLSS 5 tab.

NO WARRANTY. Not affiliated with or endorsed by THQ, Yuke's, Zuffa or the UFC.
