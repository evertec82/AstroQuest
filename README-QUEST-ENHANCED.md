# AstroQuest Enhanced standalone Quest preview

This is an experimental standalone Quest 3 build from the PC fork's shared emulator source. It runs on the headset without a PC connection during play. It has not yet been launched or performance-tested on a Quest.

## Included changes and limits

- Full-refresh pacing is now selected by default; the Quest launcher previously ignored `pace=1`. This removes a forced pacing divisor, not the headset runtime's reprojection, and does not guarantee native refresh-rate performance.
- Shared game timing, physics timing safeguards, renderer fixes and recovery changes are compiled into the ARM64 core.
- The guest timer scheduler now has a portable condition-variable implementation. Windows high-resolution timer APIs and busy-spin tuning do not transfer to Quest; this implementation still needs on-device timing validation.
- The shared transparency corrections are included, but the general NVIDIA coverage-reduction extension is unavailable on Quest's Adreno GPU. The fallback paths compile; equivalent visual results are not yet verified.
- PC OpenXR queue changes, desktop mirror throttling, the Windows launcher and its crash reporter do not operate in this standalone app. The existing Quest OpenXR host, hand/controller support, runtime and Turnip driver are retained. A separate Quest log ZIP collector is provided.

## Install

1. Enable developer mode and USB debugging on your Quest 3. Connect it by USB and authorize the computer in the headset.
2. Sideload `AstroQuest-Standalone-0.8.5-preview.apk` with SideQuest or `adb install -r AstroQuest-Standalone-0.8.5-preview.apk`.
3. The app is named **AstroQuest Enhanced (preview)** under Unknown Sources. Its package is `com.astrobotquest.vrhost.evertec82`, separate from upstream AstroQuest. Do not uninstall upstream to install this preview.
4. Supply your own extracted Astro Bot game. The upstream layout is `/data/local/tmp/astro/games/CUSA12392/eboot.bin` with the game's remaining extracted files alongside it. For example, `adb push CUSA12392 /data/local/tmp/astro/games/`. An existing game in that shared location can be reused. No game files are included.
5. Alternatively use `/sdcard/Android/data/com.astrobotquest.vrhost.evertec82/files/games/CUSA12392/` after the first launch creates the app folder.

The preview has separate app data and saves. Upstream saves are not automatically migrated. Keep upstream installed until the preview is validated. The APK uses a local preview signing key, not upstream's release key.

## Pacing and diagnostics

Optional settings go in `/sdcard/Android/data/com.astrobotquest.vrhost.evertec82/files/vrhost.txt`. Use `pace=0` for upstream steady 30 FPS pacing or `pace=1` for full-refresh mode. The existing in-headset FOV menu is retained. See README-QUEST-VR.md for other upstream settings and controller mappings.

After an issue, run `Collect-Quest-Logs.ps1` with ADB on PATH, or pass `-Adb` with its full path. It copies current and previous host/core logs and creates a ZIP. Review logcat before sharing; it can contain information from other apps. Include the level, headset, selected FOV/resolution and whether the issue occurs with `pace=0` as well.

## Build provenance

The emulator core and native/Java host are rebuilt from source. The unchanged Linux support libraries, loader and Turnip driver come from bigmak94/AstroQuest v0.21.1's Quest APK (SHA-256 `892201225d4179949f2159731762263716ecca17b2b3c3349a2deeeb94022390`). The old emulator binary is replaced with the rebuilt ARM64 binary inside that runtime. FEX is pinned to `f2b679f6028ce1c38875233aecfcf5d3f8ebecec`. No PC executable is used by the APK.

## Startup resolution selector
The startup Display settings panel now has FOV and resolution sliders. Use up/down on a headset stick or gamepad D-pad to select a row, then left/right to adjust. Confirm with A/X or a trigger. Choices are saved when starting; vrhost.txt can seed them on subsequent launches.
Resolution choices: Auto (up to 2160 x 2304), Game controlled, 816 x 870, 960 x 1080, 1200 x 1280, 1440 x 1536, 1680 x 1792, 1920 x 2048, and 2160 x 2304 per eye. Auto retains the original three lower-resolution fallback presets; the top preset is enlarged. The maximum is 50% higher per dimension (2.25 times the pixels). These use the existing game scene-resolution presets, independently of the FOV setting and compositor output size.

## v0.8.5 upstream merge

Includes upstream 0.21 and 0.21.1: optimized shader constants, eye copies and cache lookups; foveated rendering and FSR 1 options; the inter-eye depth synchronization fix; and texture-cache double-unregister protection. Startup also offers detail, frame pacing and refresh-rate choices. Full framerate remains the default in this fork; Steady 30 FPS is selectable. Higher resolution settings can reduce framerate. This merged build has passed build and automated checks but has not yet been tested in the headset.
