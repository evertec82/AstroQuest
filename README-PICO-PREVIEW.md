# AstroQuest PICO standalone preview

**Experimental PICO preview updated to the Quest v0.8.6 core and upstream 0.21.1.**

Experimental standalone Android/OpenXR build of Astro Bot, based on the enhanced Quest ARM64 core. The app is named **AstroQuest PICO (preview)**, package `com.astrobotquest.vrhost.evertec82.pico`. It keeps separate saves and settings from the Quest app.

## Install and game files

Install `AstroQuest-PICO-0.8.6-preview.apk` using ADB or your headset's sideloading tools. Enable USB debugging and authorize your computer first.

Supply your own extracted game folder, including all files, at `/data/local/tmp/astro/games/CUSA12392/eboot.bin`. Create `/data/local/tmp/astro/games` before running `adb push CUSA12392 /data/local/tmp/astro/games/`. Alternatively, after first launch use `/sdcard/Android/data/com.astrobotquest.vrhost.evertec82.pico/files/games/CUSA12392/`. Game files and firmware are not included in the APK.

Settings and current/previous host/core logs are in `/sdcard/Android/data/com.astrobotquest.vrhost.evertec82.pico/files/`. Save data is under `data/shadPS4/home/1000/savedata/CUSA12392` there. Use `Collect-PICO-Logs.ps1` to collect a report; inspect logs before sharing.

## Changes and compatibility

- Includes upstream's inter-eye depth-clear synchronization fix. The PICO-only base-mip sampling workaround remains necessary for startup rendering; it can increase distant shimmer. CPU frame-copy diagnostics are not enabled.
- Separate PICO VR manifest and app identity.
- PICO Neo3, PICO4 and PICO Ultra controller bindings, enabled only when the runtime supports their extensions.
- The enhanced shared ARM64 emulator core and full-refresh pacing default from the Quest preview. Actual frame rate depends on the scene and hardware.
- Optional Meta-specific OpenXR features are used only when advertised; they are not required for PICO startup.
- GPU support depends on the bundled Turnip driver, not just Android or OpenXR compatibility. A successful install does not establish compatibility with every PICO headset.
- The NVIDIA-only transparency coverage extension cannot operate on Adreno. Visual equivalence with the PC build is not established.

Local validation status is recorded separately from this distributable document. No unpublished device specifications are included here. This is a preview, not a generally validated PICO release.

## Startup display settings (0.8.6)

Use Up/Down to choose FOV or resolution, Left/Right to adjust, and A/X or a trigger to play. Both choices are saved. Resolution includes Auto, Game controlled, and fixed per-eye sizes of 816 x 870, 960 x 1080, 1200 x 1280, 1440 x 1536, 1680 x 1792, 1920 x 2048, and 2160 x 2304. Auto reaches 2160 x 2304 while retaining its original lower-resolution fallbacks. The maximum has 2.25 times the former pixel count, which may reduce performance. Includes the upstream black-square fix, shader and cache optimizations, distant detail/foveation/FSR 1/refresh controls, full-framerate or steady-30 pacing, and the game 1.04 soccer timing correction.
