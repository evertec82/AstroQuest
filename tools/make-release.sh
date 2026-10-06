#!/usr/bin/env bash
# Packs the files of a GitHub release into build/release/:
#   AstroQuest-<version>-Quest3.apk           the headset app, build/quest/astro-vr-host.apk
#   AstroQuest-<version>-PC-VR-Windows.zip    the PC play folder: the emulator from
#                                             build/win-x64, the launcher, clean settings, and
#                                             PkgTool (tools/pkgtool) for unpacking a game
#                                             package; no game, saves, logs or caches
#   SHA256SUMS.txt
#   tools/make-release.sh <version>       (the version must be the APK's versionName)
# tools/pkgtool is PkgTool-0.2.231.zip of https://github.com/maxton/LibOrbisPkg/releases/tag/v0.2
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
version=${1:?usage: tools/make-release.sh <version>}
python=${PYTHON:-$(cat "$root/tools/python.local" 2>/dev/null || command -v python3 || command -v python)}
aapt2="${ANDROID_SDK:-$LOCALAPPDATA/Android/Sdk}/build-tools/36.1.0/aapt2.exe"
name="AstroQuest-$version"
out="$root/build/release"
pc="$out/$name-PC-VR-Windows"

apk="$root/build/quest/astro-vr-host.apk"
apk_version=$("$aapt2" dump badging "$apk" | sed -n "s/.*versionName='\([^']*\)'.*/\1/p")
[[ "$apk_version" == "$version" ]] || {
  echo "build/quest/astro-vr-host.apk is version $apk_version, not $version" >&2; exit 1; }

rm -rf "$out"
# The first user's home: without it the Windows build asks at its first start whether to move
# saves over from where an older shadPS4 kept them, in a message box behind the game.
mkdir -p "$pc/pc-vr/user/input_config" "$pc/games" \
         "$pc/pc-vr/user/home/1000/"{savedata,trophy,inputs}
cp "$apk" "$out/$name-Quest3.apk"

cp "$root/build/win-x64/shadps4.exe" "$pc/pc-vr/"
cp "$root/pc-vr/launch.ps1" "$pc/pc-vr/"
cp "$root/Play Astro Bot VR.bat" "$pc/"
cp "$root/pc-vr/user/input_config/default.ini" "$root/pc-vr/user/input_config/global.ini" \
   "$pc/pc-vr/user/input_config/"
# What the launcher unpacks a game package with.
mkdir -p "$pc/pc-vr/pkgtool"
cp "$root/tools/pkgtool/PkgTool.exe" "$root/tools/pkgtool/LibOrbisPkg.dll" \
   "$root/tools/pkgtool/LICENSE.txt" "$pc/pc-vr/pkgtool/"
cat > "$pc/pc-vr/pkgtool/README.txt" <<'EOF'
PkgTool and LibOrbisPkg 0.2.231, by Maxton, unchanged: the launcher uses them to unpack a game
package (.pkg). Licensed under the GNU Lesser General Public License, version 3 (LICENSE.txt).
Source: https://github.com/maxton/LibOrbisPkg (release v0.2).
EOF
# The settings file as documented: what the launcher's window saved here is left out.
sed '/^[a-z_]*=/d' "$root/pc-vr/settings.txt" > "$pc/pc-vr/settings.txt"
# The emulator's settings as the play folder has them, with everything that names this PC's
# folders or devices back at its default.
"$python" - "$(cygpath -m "$root/pc-vr/user/config.json")" \
    "$(cygpath -m "$pc/pc-vr/user/config.json")" <<'EOF'
import json, sys
config = json.load(open(sys.argv[1], encoding="utf-8"))
config["General"].update({"addon_install_dir": "", "font_dir": "", "home_dir": "",
                          "install_dirs": [], "sys_modules_dir": "", "shadnet_server": ""})
config["Input"].update({"default_controller_id": "", "camera_id": -1,
                        "motion_controls_enabled": True, "background_controller_input": True})
config["Vulkan"].update({"gpu_id": -1, "vkvalidation_enabled": False, "renderdoc_enabled": False})
config["Log"].update({"type": "file", "sync": False, "filter": "*:Info", "append": False})
for key in [k for k in config["Audio"] if k.endswith("_device")]:
    config["Audio"][key] = "Default Device"
json.dump(config, open(sys.argv[2], "w", encoding="utf-8"), indent=2)
EOF

cat > "$pc/games/PUT YOUR GAME HERE.txt" <<'EOF'
Put your own copy of ASTRO BOT Rescue Mission (European release CUSA12392, version 1.00 or
1.04) in this folder, then start "Play Astro Bot VR.bat". Either form will do, anywhere in
here:

- the game's folder, the one with eboot.bin in it, or
- the game's .pkg file: it is unpacked the first time, which takes a minute and about 13 GB.
  (Only a package made from a dump of the game can be unpacked. One downloaded from the
  PlayStation Store is encrypted and cannot be used. A package that is only the game's
  update is not the game: see "The game's versions" in README-PC-VR.md on GitHub.)

If the game is somewhere else, just start: a window asks where it is.
EOF
cat > "$pc/README.txt" <<EOF
AstroQuest $version - ASTRO BOT Rescue Mission in VR, played on this PC and shown in a headset:
a Meta Quest through Virtual Desktop, or a PC headset through SteamVR or an OpenXR runtime of
its own. https://github.com/bigmak94/AstroQuest

1. Put your own copy of the game in the games folder: its folder (the one with eboot.bin in
   it) or its .pkg file, which is unpacked the first time. Or skip this: a window asks where
   the game is. Keep this folder's path short, e.g. C:\Games\AstroQuest: the emulator cannot
   open the game's files whose full path would be longer than 260 characters.
2. Virtual Desktop: install the Streamer on this PC and choose VDXR as the OpenXR runtime in
   its Options. In the headset, set Virtual Desktop's frame rate to 120 (Streaming settings).
   (With SteamVR instead: start it and see that the headset is ready.)
3. Connect the DualSense to this PC (USB cable, or Bluetooth paired with the PC, not with the
   headset). The headset's own controllers play too: without a gamepad, and with one
   whenever they were used after it.
4. Connect Virtual Desktop to this PC, then start "Play Astro Bot VR.bat": its window says
   what does what, and has the game's language to choose (Windows' own unless you say
   otherwise). (If the Microsoft Visual C++ runtime is missing, it says so and offers
   Microsoft's download.)

In the game: hold the controller where the outline is on the first screen; look at a planet
and press X to choose it; hold OPTIONS for a second to reset the view. With the headset's
controllers or a gamepad without a touchpad: right trigger presses the touchpad, right grip
(R1) swipes forward, left trigger pulls back and lets go (the catapult at the end of a level).
To turn round without turning yourself: hold L1 (the headset's controllers: the left grip)
and flick the right stick to a side.
Settings are in pc-vr\settings.txt, the log in pc-vr\user\log\shad_log.txt, saves in
pc-vr\user\home.

AstroQuest is free software under the GNU GPL, version 2 or later (LICENSE.txt); the source is
at the address above. It contains no part of the game: use it only with a game you own.
EOF
tr -d '\r' < "$root/LICENSE" > "$pc/LICENSE.txt"
cp "$root/THIRD-PARTY-NOTICES.md" "$pc/THIRD-PARTY-NOTICES.md"

"$python" - "$(cygpath -m "$out")" "$name-PC-VR-Windows" <<'EOF'
import os, sys, zipfile
out, folder = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(os.path.join(out, folder + ".zip"), "w", zipfile.ZIP_DEFLATED,
                     compresslevel=9) as archive:
    for here, folders, files in sorted(os.walk(os.path.join(out, folder))):
        if not folders and not files:
            archive.write(here, os.path.relpath(here, out).replace(os.sep, "/") + "/")
        for name in sorted(files):
            path = os.path.join(here, name)
            archive.write(path, os.path.relpath(path, out).replace(os.sep, "/"))
EOF
(cd "$out" && sha256sum "$name-Quest3.apk" "$name-PC-VR-Windows.zip" > SHA256SUMS.txt)
ls -la "$out"
cat "$out/SHA256SUMS.txt"
