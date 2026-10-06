#!/usr/bin/env bash
# Does the emulator survive Windows changing the device it plays on by default, the way a
# program that streams to a headset changes it when the headset connects? Plays the game
# (flat, no headset) with its sound meant for the device that is Windows' default now, makes
# another one the default after a while and the first one again after that, and says whether
# the emulator is still running and where its ports went.
#   tools/pc-audio-default-test.sh "<other device>" [emulator exe] [seconds before the change]
# SETTING="Default Device" leaves the sound to the system instead of naming the first device.
# Both devices are audible meanwhile. Names: tools/audio-endpoint.ps1 list
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
other=${1:?the name of the other device}
exe=${2:-$root/build/win-x64/shadps4.exe}
warmup=${3:-35}
python=${PYTHON:-$(cat "$root/tools/python.local" 2>/dev/null || command -v python3 || command -v python)}
endpoint() {
    powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$root/tools/audio-endpoint.ps1")" "$@" | tr -d '\r'
}

cd "$root/build/win-x64" || exit 1
first=$(endpoint default)
[ -n "$first" ] || { echo "Windows has no default playback device" >&2; exit 1; }
# The device Windows would play on if all were there: one that is not there now (a headset's,
# while nothing streams to it) has to be the preferred one again afterwards, or Windows no
# longer changes over to it when it comes back. (It did not, after this test's first runs.)
preferred=$(endpoint preferred)
config=user/config.json
cp "$config" "$config.audio-test"
restore() {
    endpoint default "$first" > /dev/null
    if [ -n "$preferred" ] && [ "$preferred" != "$first" ]; then
        endpoint default "$preferred" > /dev/null
    fi
    mv "$root/build/win-x64/$config.audio-test" "$root/build/win-x64/$config"
}
trap restore EXIT
"$python" - "$config" "${SETTING:-$first}" <<'EOF'
import json, sys
config = json.load(open(sys.argv[1], encoding="utf-8"))
config["Audio"]["sdl_main_output_device"] = sys.argv[2]
json.dump(config, open(sys.argv[1], "w", encoding="utf-8"), indent=2)
EOF

export SHADPS4_VR_DEMO=1 SHADPS4_OPENXR=0
"$exe" -g "$(cygpath -w "$root/games/CUSA12392/eboot.bin")" > audio-default-test.log 2>&1 &
game=$!
running() { kill -0 "$game" 2>/dev/null && echo "the emulator is running" || echo "THE EMULATOR HAS STOPPED"; }

sleep "$warmup"
echo "$warmup s: $(running); Windows plays on: $first"
echo "now on: $(endpoint default "$other")"
sleep 14
echo "14 s later: $(running)"
echo "and back on: $(endpoint default "$first")"
sleep 14
echo "14 s later: $(running)"
kill "$game" 2>/dev/null
wait "$game" 2>/dev/null
grep -a -E "Opened audio device|sound moves|went away|now plays on|Audio output (added|removed)|Unhandled Exception|SDL_GetAudioStream" user/log/shad_log.txt |
    sed -E 's/^\[[^]]*\] <[A-Za-z]+> \(([^)]*)\) [^ ]+ [A-Za-z_]+: /\1 | /' | cut -c1-200
