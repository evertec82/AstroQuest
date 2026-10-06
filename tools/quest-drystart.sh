#!/usr/bin/env bash
# Starts the installed app's own activity on a connected Quest without showing it, which needs
# nobody to wear or unlock the headset: the activity is created and runs everything it runs
# when the app is launched (settings, log files, the OpenXR session up to "idle", the runtime,
# the emulator with the game, sound, the status panel's text), for the given seconds, and is
# then destroyed. What is not reached is what needs a display: the running session, the
# permission question, the controller.
#
#   tools/quest-drystart.sh [seconds, default 60] [settings file]
#
# A settings file (lines as in vrhost.txt) is used in place of the one on the headset.
#
# The app's host.log and core.log land in build/quest/drystart/.
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ADB="$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe"
export ANDROID_SERIAL=${ANDROID_SERIAL:-$(cat "$(dirname "${BASH_SOURCE[0]}")/quest-serial.local" 2>/dev/null)}
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
# (ASTRO_PACKAGE: the build made by quest-host/build.sh --test-package, if that is the
# one to run.)
package=${ASTRO_PACKAGE:-com.astrobotquest.vrhost}
seconds=${1:-60}
out="$root/build/quest/drystart"
files=/sdcard/Android/data/$package/files

settings=""
if [ -n "${2:-}" ]; then
  "$ADB" push "$2" /data/local/tmp/astro/vrhost-test.txt > /dev/null
  "$ADB" shell "chmod 644 /data/local/tmp/astro/vrhost-test.txt"
  settings="-e settings /data/local/tmp/astro/vrhost-test.txt"
fi
"$ADB" shell "am instrument -w -e drystart $seconds $settings $package/com.astrobotquest.vrhost.SandboxShell" > /dev/null
mkdir -p "$out"
for name in host.log core.log; do
  # (Not "adb pull": with path conversion off, adb.exe is given a path it cannot write to.)
  "$ADB" exec-out "cat $files/$name" > "$out/$name" 2>/dev/null
  [ -s "$out/$name" ] || echo "no $name"
done
# The pictures of the status panel, as the activity drew them.
for number in 0 1 2 3 4 5 6 7 8 9 10 11; do
  "$ADB" exec-out "run-as $package cat files/drystart/status-$number.png" \
    > "$out/status-$number.png" 2>/dev/null
  [ -s "$out/status-$number.png" ] || rm -f "$out/status-$number.png"
done
grep -v "starting core" "$out/host.log" | cut -c1-220
echo "core.log: $(grep -a -c "<Critical>" "$out/core.log") critical lines, last frame stats:"
grep -a "frame stats" "$out/core.log" | tail -1 | sed 's/\x1b\[[0-9;]*m//g; s/.*EndFrame: //' | cut -c1-200
