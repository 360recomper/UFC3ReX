UFC3 Recomp - DLSS 5 add-on (optional)
======================================

What it is
----------
The scripts and settings templates the launcher's DLSS 5 tab uses to start the game through NVIDIA
DLSS 5 neural rendering. Your normal Play is not changed; DLSS 5 is a second way to start the same
ufc3.exe. Saves and mods are shared. DLSS 5 is experimental third-party software, not an NVIDIA release.
No third-party files are in this zip.

You need
--------
- An NVIDIA GeForce RTX card and a current driver. NVIDIA Smooth Motion off for this game.
- UFC3 Recomp set up with UFC3 Launcher.exe.
- The DLSS 5 files, which you supply yourself (no third-party file is in our zips):
    ReShade 6.8.0 with add-on support   ReShade64.dll, ReShade64.json      https://reshade.me
    DLSS5-Feeder 1.19.0                 dlss5-feed-helper.addon64,
                                        host64\dlss5-feed-host64.exe,
                                        reshade-shaders\Shaders\DLSS5_Feed.fx  github.com/jlrouzies-fr/DLSS5-Feeder
    OptiScaler-NR 0.8.92                OptiScaler.dll, OptiScaler.ini,
                                        OptiScaler\ (dll folder)
                                        github.com/jlrouzies-fr/OptiScaler-DLSSNR-PreSR-Multipass
    NVIDIA DLSS 310.9.16                nvngx_dlss.dll                      github.com/NVIDIA/DLSS
    DLSS 5 neural runtime 310.9.16      nvngx_dlssnr.dll (RTX 20/30/40: the compatibility build)
    LumeniteFX                          Shaders\lumenite_Kernel.fx + Shaders\include\,
                                        Textures\                           github.com/umar-afzaal/LumeniteFX
    reshade-shaders (slim branch)       Shaders\ReShade.fxh                 github.com/crosire/reshade-shaders

Install
-------
1. Unzip this into the UFC3 Recomp folder (so dlss5\ sits next to ufc3.exe).
2. Put all the downloads above in one folder, unzipped, any layout (or drop them into dlss5\).
3. UFC3 Launcher.exe > DLSS 5 tab > Set up DLSS 5..., pick that folder (or dlss5\). The launcher finds each
   file by name and puts it where it belongs; the tab lists anything still missing and where it comes from.
4. Pick your DLSS 5 look in the same tab, then press Play with DLSS 5.

What setup builds (dlss5\ next to ufc3.exe; the launcher checks the starred files):
  * ReShade64.dll, * ReShade64.json, * dlss5-feed-helper.addon64
  host64\  * dlss5-feed-host64.exe, * dxgi.dll (= a copy of ReShade64.dll), * winmm.dll (= OptiScaler.dll),
           * OptiScaler.ini, * nvngx_dlss.dll, * nvngx_dlssnr.dll, OptiScaler\ (OptiScaler's dll folder)
  shaders\Shaders\  * DLSS5_Feed.fx, * lumenite_Kernel.fx, * ReShade.fxh, include\lumenite_*.fxh
  shaders\Textures\  lumenite_bluenoise256.png
  * templates\ReShade.template.ini, * templates\ReShadePreset.ini, * templates\dlss5-feed.template.cfg,
  * align-dlss5-panel.ps1   (these four come from this add-on zip / the recomp zip)
A new OptiScaler.ini gets the DLSS 5 settings set for you (Dx12Upscaler=dlss, neural pass on, working scale 0.5).

In game: F9 DLSS 5 on/off. F4 recomp menu.
