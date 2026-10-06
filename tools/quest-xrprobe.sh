#!/usr/bin/env bash
# Asks the headset's OpenXR runtime what can be asked without anybody wearing the headset, from
# inside the installed app (its SandboxShell instrumentation):
#
#   tools/quest-xrprobe.sh [level]
#
#   1  the app finds the runtime; which of the extensions the host uses it has
#   2  also: an instance is made, and the system, the recommended eye size and the names of the
#      runtime's performance counters are listed
#   3  (default) the host itself runs for five seconds: session, swapchains, hand trackers and
#      shared frame buffers are created as in a real start, the session (which stays idle) is
#      asked for the refresh rates, the performance levels and the thread hints, and a test
#      picture is copied into the swapchain's images and read back
#
# The log lands in build/quest/xrprobe.log. A session itself (frames, tracking, the display) is
# what this cannot reach.
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ADB="$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe"
export ANDROID_SERIAL=${ANDROID_SERIAL:-$(cat "$(dirname "${BASH_SOURCE[0]}")/quest-serial.local" 2>/dev/null)}
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
# (ASTRO_PACKAGE: the build made by quest-host/build.sh --test-package, if that is the
# one to run.)
package=${ASTRO_PACKAGE:-com.astrobotquest.vrhost}
level=${1:-3}

"$ADB" shell "am instrument -w -e xrprobe $level $package/com.astrobotquest.vrhost.SandboxShell" \
  | sed -e 's/^INSTRUMENTATION_RESULT: stream=//' -e '/^INSTRUMENTATION_CODE/d'
mkdir -p "$root/build/quest"
"$ADB" exec-out "run-as $package cat files/xrprobe.log" > "$root/build/quest/xrprobe.log" 2>/dev/null
grep -v "xr probe: extensions:" "$root/build/quest/xrprobe.log" | cut -c1-240
