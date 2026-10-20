# AstroQuest Galaxy XR 0.8.7 experimental preview

Standalone Android XR build. OpenXR compatibility does not guarantee the bundled FEX/Turnip emulator runtime will work on this device. No game files are included.

## Install

Enable USB debugging on the Galaxy XR and authorize your computer. Install using:

    adb install -r AstroQuest-GalaxyXR-0.8.7-preview.apk

Tracked controllers use the standard Touch interaction profile. Hand-only input is not enabled in this preview.

Supply your own extracted Astro Bot game folder (not a PKG or archive), including eboot.bin and all game data:

    adb shell mkdir -p /data/local/tmp/astro/games
    adb push CUSA12392 /data/local/tmp/astro/games/

The resulting path must be /data/local/tmp/astro/games/CUSA12392/eboot.bin. Open AstroQuest Galaxy XR (preview) in the headset. Keep a complete, consistent game version; do not mix incomplete update files with a base install.

## Included settings and fixes

Uses the Quest v0.8.7 ARM64 core with upstream 0.21.1 changes, including inter-eye depth synchronization, shader/cache optimizations, and the 1.04 soccer timing correction. Includes FOV and resolution sliders, Auto up to 2160 x 2304 per eye, full-refresh pacing, distant detail, foveation, FSR 1 and refresh controls. Optional runtime features only work where supported. No PICO base-mip workaround is enabled. The PC NVIDIA-only transparency fix cannot run on this Adreno path.

Separate app identity: com.astrobotquest.vrhost.evertec82.galaxyxr. Existing Quest/PICO installations and their saves are not replaced.

## Troubleshooting

Run Collect-GalaxyXR-Logs.ps1 with ADB available, or supply -Adb with its path and -Serial if multiple devices are connected. It creates a report ZIP with host/core logs, Android logcat and package information. Review before sharing. Report whether startup settings appear, whether the game starts, and any black squares or corrupted textures.

Settings/logs: /sdcard/Android/data/com.astrobotquest.vrhost.evertec82.galaxyxr/files/
Saves: data/shadPS4/home/1000/savedata/CUSA12392 beneath that directory.

Android XR manifest requirements: https://developer.android.com/develop/xr/openxr/get-started

## v0.8.7
Includes upstream v0.22 core fixes and the selectable 45 FPS cap alongside full framerate and steady 30 FPS. A 45 FPS cap aligns to display refresh: 45 at 90 Hz, 36 at 72 Hz, or 40 at 120 Hz. Existing FOV, resolution and platform settings are retained.
