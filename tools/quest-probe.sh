#!/usr/bin/env bash
# Builds one of the small C probes in tools/probes for the emulator's Linux runtime and runs it
# on a connected Quest inside the host app's sandbox, with the same loader, libraries, GPU driver
# and driver shim the emulator core gets there.
#
#   tools/quest-probe.sh <probe.c> [arguments...]
#
#   ASTRO_DRIVER   folder of the Vulkan driver inside the app's files folder
#                  (default runtime/drivers/turnip)
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
ADB="$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe"
# The headset also shows up over the network: name the USB connection.
export ANDROID_SERIAL=${ANDROID_SERIAL:-$(cat "$(dirname "${BASH_SOURCE[0]}")/quest-serial.local" 2>/dev/null)}
# (ASTRO_PACKAGE: the build made by quest-host/build.sh --test-package, if that is the
# one to run.)
package=${ASTRO_PACKAGE:-com.astrobotquest.vrhost}
stage=/data/local/tmp/astro
driver=${ASTRO_DRIVER:-runtime/drivers/turnip}

source=${1:?usage: quest-probe.sh <probe.c> [arguments...]}
shift
name=$(basename "$source" .c)

cd "$root" || exit 1
mkdir -p build/quest/probes
tools/llvm/bin/aarch64-linux-gnu-clang.exe -O1 -Wall \
  -I shadps4-arm64-main/externals/vulkan-headers/include \
  "$source" -o "build/quest/probes/$name" \
  build/quest/runtime/host/libvulkan.so.1 -lpthread || exit 1

"$ADB" push "build/quest/probes/$name" "$stage/$name" > /dev/null || exit 1
"$ADB" shell "chmod 644 $stage/$name"

# The loader is not the first word after env: the app folder has "=" in its name, which env
# would take for a variable assignment.
command="
cp $stage/$name ./$name && chmod 700 ./$name
env HOME=\$PWD/home TMPDIR=\$PWD/tmp GLIBC_TUNABLES=glibc.pthread.rseq=0 \
  LD_LIBRARY_PATH=\$PWD/runtime/host:\$PWD/$driver \
  VK_ICD_FILENAMES=\$PWD/$driver/freedreno_icd.aarch64.json \
  timeout 120 \$ASTRO_LIB_DIR/libastro_ld.so --library-path \$PWD/runtime/host:\$PWD/$driver \
  --preload \$PWD/runtime/host/libkgsl_compat.so ./$name $* 2>&1
"
"$ADB" shell "am instrument -w -e cmd '$command' $package/com.astrobotquest.vrhost.SandboxShell" \
  | sed -e 's/^INSTRUMENTATION_RESULT: stream=//' -e '/^INSTRUMENTATION_CODE/d'
