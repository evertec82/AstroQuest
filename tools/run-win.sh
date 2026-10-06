#!/usr/bin/env bash
# Runs the Windows dev build of the core against the extracted game for a fixed time and keeps
# the log, so HLE changes can be checked without a headset.
#   tools/run-win.sh [seconds] [extra shadps4 args...]
# SHADPS4_GAME=<folder> runs another copy of the game than games/CUSA12392.
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
seconds=${1:-40}
shift || true

# The core runs from its build folder: a script given relative to where this was started from
# would not be found there, and the run would silently play without input.
if [ -n "${SHADPS4_INPUT_SCRIPT:-}" ]; then
    if [ ! -f "$SHADPS4_INPUT_SCRIPT" ]; then
        echo "input script not found: $SHADPS4_INPUT_SCRIPT" >&2
        exit 1
    fi
    export SHADPS4_INPUT_SCRIPT=$(cygpath -w "$(realpath "$SHADPS4_INPUT_SCRIPT")")
fi

cd "$root/build/win-x64" || exit 1
mkdir -p user
export SHADPS4_VR_DEMO=${SHADPS4_VR_DEMO:-1}
game=${SHADPS4_GAME:-$root/games/CUSA12392}
timeout "$seconds" ./shadps4.exe -g "$(cygpath -w "$game/eboot.bin")" "$@" > run-stdout.log 2>&1
echo "exit=$?" >> run-stdout.log
wc -l run-stdout.log
