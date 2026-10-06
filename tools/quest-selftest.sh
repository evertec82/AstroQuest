#!/usr/bin/env bash
# Runs the host app's self test on a connected Quest: the app's own code starts the core, feeds it
# a controller and a moving head over its sockets, and reads the frames the core renders into the
# buffers the two share, exactly as it does with a headset on, minus the OpenXR session. Nobody
# has to wear the headset. The frames that arrived are pulled to build/quest/selftest/ as PNGs
# (both eyes, half size), together with the core's log.
#
#   tools/quest-selftest.sh <seconds> [NAME=value ...]     (variables for the core)
#
# ASTRO_FRESH=1 runs with the runtime as the installed app carries it (unpacked from the APK's
# assets, like on the app's first start) instead of with the build in build/arm64.
#
# HOST_... among the variables are the test's own (README-QUEST-VR.md, "Testing on the headset
# without wearing it"): a display and a compositor stand-in, where the head looks, what is
# said of the controller (HOST_PAD), how often a picture is kept (HOST_SHOTS).
#
# ASTRO_MIC=real makes the game's microphone the headset's own instead of the test signal the
# app otherwise feeds it (a second of noise every five); the app needs the permission for it
# (adb shell pm grant com.astrobotquest.vrhost android.permission.RECORD_AUDIO).
#
# Needs the app installed (quest-host/build.sh) and the game in /data/local/tmp/astro/games.
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ADB="$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe"
# The headset also shows up over the network: name the USB connection.
export ANDROID_SERIAL=${ANDROID_SERIAL:-$(cat "$(dirname "${BASH_SOURCE[0]}")/quest-serial.local" 2>/dev/null)}
# (ASTRO_PACKAGE: the build made by quest-host/build.sh --test-package, if that is the
# one to run.)
package=${ASTRO_PACKAGE:-com.astrobotquest.vrhost}
stage=/data/local/tmp/astro
seconds=${1:-60}
shift $(( $# < 1 ? $# : 1 ))
# What the app itself gives the core unless vrhost.txt says otherwise; settings given here come
# after these and replace them.
extra_env="SHADPS4_MAX_MSAA=1 TU_DEBUG=sysmem KGSL_COMPAT_PRIORITY=12 SHADPS4_THREAD_NICE=Game:Main=-14,shadPS4:GpuComm=-14,sndx_out_thread=-16,sndx_audio3d=-16,shadPS4:AudioOu=-16,shadPS4:VrHostL=-18,shadPS4:Present=-18 SHADPS4_FRAME_STATS=1 SHADPS4_AUDIOOUT_STATS=1 SHADPS4_AUDIOIN_STATS=1 SHADPS4_VR_REFRESH_RATE=90 SHADPS4_VR_SHARPEN=0.6 $*"

cd "$root" || exit 1
mkdir -p build/quest/selftest
tools/llvm/bin/llvm-strip.exe -o build/quest/shadps4-stripped build/arm64/shadps4 || exit 1
tools/llvm/bin/aarch64-linux-gnu-clang.exe -O2 -fPIC -shared quest-host/runtime/kgsl_compat.c \
  -o build/quest/libkgsl_compat.so -lpthread || exit 1

# From here on Git Bash must leave the device paths alone.
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
"$ADB" push build/quest/shadps4-stripped $stage/shadps4-arm64-fex | tail -1
"$ADB" push build/quest/libkgsl_compat.so $stage/libkgsl_compat.so > /dev/null
"$ADB" shell "chmod 644 $stage/shadps4-arm64-fex $stage/libkgsl_compat.so"

install=""
if [ -n "${ASTRO_FRESH:-}" ]; then
  install="-e install 1"
else
  # The runtime the app unpacks on its first start, with the current build of the core in it.
  # Without its stamp, so that the app unpacks its own again when it is started for real.
  "$ADB" shell "am instrument -w -e cmd 'mkdir -p runtime/host; rm -f runtime/.stamp; [ -d runtime/drivers ] || cp -r $stage/runtime/. runtime/; cp $stage/shadps4-arm64-fex runtime/host/shadps4-arm64-fex; cp $stage/libkgsl_compat.so runtime/host/libkgsl_compat.so' $package/com.astrobotquest.vrhost.SandboxShell" > /dev/null
fi

"$ADB" shell "am instrument -w -e selftest $seconds -e env '$extra_env' -e mic ${ASTRO_MIC:-test} $install $package/com.astrobotquest.vrhost.SandboxShell" \
  | sed -e 's/^INSTRUMENTATION_RESULT: stream=//' -e '/^INSTRUMENTATION_CODE/d'

rm -rf build/quest/selftest/*
"$ADB" exec-out "run-as $package sh -c 'cd files/selftest && tar -cf - *.png core.log 2>/dev/null'" \
  | tar -xf - -C build/quest/selftest 2>/dev/null
"$ADB" exec-out "run-as $package sh -c 'cd files/selftest && tar -cf - host.log 2>/dev/null'" \
  | tar -xf - -C build/quest/selftest 2>/dev/null
ls build/quest/selftest | head -40
