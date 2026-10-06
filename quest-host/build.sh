#!/usr/bin/env bash
# Builds the Quest host APK without Gradle or Android Studio projects:
#   NDK clang -> libastrovr.so, javac + d8 -> classes.dex, aapt2 -> APK, zipalign + apksigner.
#
# The Linux runtime the emulator core runs in (glibc, its loader, the Turnip Vulkan driver) is
# taken from the user's copy of the Bachata S4 APK at build time, and the core itself from
# build/arm64 (tools/build-arm64.sh).
#
#   quest-host/build.sh [--driver <zip inside the APK's assets/drivers>] [--trade-cpu-for-gpu]
#                       [--test-package]
#
# --trade-cpu-for-gpu builds the app that gives up one processor clock level (1.92 -> 1.65 GHz
# on a Quest 3) for one more GPU clock level (up to 640 MHz instead of 599). The system only
# takes that choice from the manifest, so it is a build of its own.
#
# --test-package builds the same app under another name (com.astrobotquest.vrhost.test,
# "Astro VR Host (test)", build/quest/astro-vr-host-test.apk, debug key): it installs next to
# the app proper and touches neither it nor its saves, for trying a build on a headset whose
# owner plays with the other one (the tools take ASTRO_PACKAGE for which of the two they run).
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
host="$root/quest-host"
out="$root/build/quest"

NDK=${ANDROID_NDK:-"/c/Program Files/Unity/Hub/Editor/6000.3.2f1/Editor/Data/PlaybackEngines/AndroidPlayer/NDK"}
SDK=${ANDROID_SDK:-"$LOCALAPPDATA/Android/Sdk"}
BUILD_TOOLS="$SDK/build-tools/36.1.0"
PLATFORM_JAR="$SDK/platforms/android-36/android.jar"
JDK=${JAVA_HOME:-"/c/Program Files/Android/Android Studio/jbr"}
# Python: $PYTHON, or the one named in tools/python.local (a full path), or the one on PATH.
PYTHON=${PYTHON:-$(cat "$root/tools/python.local" 2>/dev/null || command -v python3 || command -v python)}
BACHATA_APK="$root/bachatas4-0.2.4-release.apk"
CORE="$root/build/arm64/shadps4"
OPENXR="$root/tools/openxr"
driver="turnip-vauzi-7xx-EMULATOR.zip"
# Source paths in the binaries are counted from this folder, not from the machine's root.
prefix_map="-ffile-prefix-map=$(cygpath -m "$root")/="

trade=""
test_package=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --driver) driver=$2; shift 2 ;;
    --trade-cpu-for-gpu) trade=1; shift ;;
    --test-package) test_package=com.astrobotquest.vrhost.test; shift ;;
    *) echo "unknown option $1" >&2; exit 64 ;;
  esac
done

export JAVA_HOME="$JDK"
export PATH="$JDK/bin:$PATH"
for needed in "$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe" "$BUILD_TOOLS/aapt2.exe" \
              "$PLATFORM_JAR" "$JDK/bin/javac.exe" "$PYTHON" "$BACHATA_APK" "$CORE" \
              "$OPENXR/jni/arm64-v8a/libopenxr_loader.so" \
              "$root/tools/llvm/bin/aarch64-linux-gnu-clang.exe"; do
  [[ -e "$needed" ]] || { echo "missing: $needed" >&2; exit 1; }
done

rm -rf "$out/apk" "$out/classes" "$out/dex" "$out/runtime" "$out/bachata"
mkdir -p "$out/apk/lib/arm64-v8a" "$out/apk/assets" "$out/classes" "$out/dex" "$out/runtime" "$out/bachata"

echo "== runtime"
# The glibc world: libraries and the loader (which Android only lets us execute from the
# app's native library folder, hence its lib*.so name).
unzip -q -o "$BACHATA_APK" "assets/runtime/runtime.zip" "assets/drivers/$driver" \
  "lib/arm64-v8a/libbachata_host_loader.so" -d "$out/bachata"
unzip -q -o "$out/bachata/assets/runtime/runtime.zip" "host/*" -d "$out/runtime"
rm -rf "$out/runtime/host/shadps4-arm64-fex" "$out/runtime/host/fexcore-guest-harness" \
       "$out/runtime/host/fexcore-smoke" "$out/runtime/host/lib" "$out/runtime/host/vulkan"
cp "$out/bachata/lib/arm64-v8a/libbachata_host_loader.so" "$out/apk/lib/arm64-v8a/libastro_ld.so"
mkdir -p "$out/runtime/drivers/turnip"
unzip -q -o "$out/bachata/assets/drivers/$driver" -d "$out/runtime/drivers/turnip"
[[ -f "$out/runtime/drivers/turnip/freedreno_icd.aarch64.json" ]] || {
  echo "$driver is not a glibc Turnip package (no ICD manifest)" >&2; exit 1; }
"$root/tools/llvm/bin/llvm-strip.exe" -o "$out/runtime/host/shadps4-arm64-fex" "$CORE"
# Translates the GPU memory calls Horizon OS denies to apps into the ones it allows. A glibc
# library like the rest of the runtime, so it is built with the same cross compiler as the core.
"$root/tools/llvm/bin/aarch64-linux-gnu-clang.exe" -O2 -fPIC -shared "$prefix_map" \
  "$(cygpath -m "$host/runtime/kgsl_compat.c")" -lpthread \
  -o "$(cygpath -m "$out/runtime/host/libkgsl_compat.so")"

"$PYTHON" - "$out/runtime" "$out/apk/assets/runtime.zip" "$out/apk/assets/runtime.stamp" <<'PY'
import hashlib, os, sys, zipfile
source, target, stamp = sys.argv[1:4]
digest = hashlib.sha256()
with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
    for folder, _, files in sorted(os.walk(source)):
        for name in sorted(files):
            path = os.path.join(folder, name)
            relative = os.path.relpath(path, source).replace(os.sep, "/")
            with open(path, "rb") as handle:
                data = handle.read()
            digest.update(relative.encode() + b"\0" + data)
            # A fixed timestamp keeps the archive, and so the stamp, reproducible.
            info = zipfile.ZipInfo(relative, (2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o755 << 16
            archive.writestr(info, data)
with open(stamp, "w") as handle:
    handle.write(digest.hexdigest() + "\n")
print("runtime.zip", os.path.getsize(target) // (1 << 20), "MB")
PY

echo "== native"
"$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe" --target=aarch64-linux-android32 \
  -std=c++20 -O2 -fPIC -fvisibility=hidden "$prefix_map" -Wall -Wextra -Wno-unused-parameter \
  -Wno-missing-field-initializers \
  -I "$(cygpath -m "$OPENXR/prefab/modules/headers/include")" \
  -I "$(cygpath -m "$root/shadps4-arm64-main/src/core/vr")" \
  -I "$(cygpath -m "$root/shadps4-arm64-main/src/input")" \
  "$(cygpath -m "$host/cpp/main.cpp")" "$(cygpath -m "$host/cpp/core_process.cpp")" \
  "$(cygpath -m "$host/cpp/xr_host.cpp")" "$(cygpath -m "$host/cpp/gl_frames.cpp")" \
  "$(cygpath -m "$host/cpp/self_test.cpp")" "$(cygpath -m "$host/cpp/log.cpp")" \
  "$(cygpath -m "$host/cpp/xr_probe.cpp")" \
  -shared -static-libstdc++ -Wl,--no-undefined -Wl,-soname,libastrovr.so \
  -L "$(cygpath -m "$OPENXR/jni/arm64-v8a")" -lopenxr_loader \
  -lEGL -lGLESv3 -landroid -lnativewindow -laaudio -llog -lz \
  -o "$(cygpath -m "$out/apk/lib/arm64-v8a/libastrovr.so")"
cp "$OPENXR/jni/arm64-v8a/libopenxr_loader.so" "$out/apk/lib/arm64-v8a/"

echo "== java"
javac -Xlint:-options -source 11 -target 11 -classpath "$(cygpath -m "$PLATFORM_JAR")" \
  -d "$(cygpath -m "$out/classes")" \
  "$(cygpath -m "$host/java/com/astrobotquest/vrhost/MainActivity.java")" \
  "$(cygpath -m "$host/java/com/astrobotquest/vrhost/SandboxShell.java")" \
  "$(cygpath -m "$host/java/com/astrobotquest/vrhost/RuntimeInstaller.java")"
# d8 is a batch file around a jar; call the jar so paths with spaces survive.
java -cp "$(cygpath -m "$BUILD_TOOLS/lib/d8.jar")" com.android.tools.r8.D8 \
  --lib "$(cygpath -m "$PLATFORM_JAR")" --min-api 32 --output "$(cygpath -m "$out/dex")" \
  $(find "$out/classes" -name "*.class" -exec cygpath -m {} \;)
cp "$out/dex/classes.dex" "$out/apk/classes.dex"

echo "== package"
manifest="$host/AndroidManifest.xml"
if [[ -n "$trade" ]]; then
  # Per Meta's "Trading CPU and GPU levels": 1 = one level more for the GPU, one less for the
  # processor.
  manifest="$out/AndroidManifest.xml"
  sed 's|^\( *\)<activity$|\1<meta-data\n\1    android:name="com.oculus.trade_cpu_for_gpu_amount"\n\1    android:value="1" />\n\n\1<activity|' \
    "$host/AndroidManifest.xml" > "$manifest"
  grep -q trade_cpu_for_gpu_amount "$manifest" || { echo "could not add the trade to the manifest" >&2; exit 1; }
  echo "   with one processor level traded for one GPU level"
fi
res="$host/res"
rename=()
apk="$out/astro-vr-host.apk"
if [[ -n "$test_package" ]]; then
  # Another name for the system and for the eye; the classes keep theirs.
  res="$out/res-test"
  rm -rf "$res"
  cp -r "$host/res" "$res"
  sed -i 's|>Astro VR Host<|>Astro VR Host (test)<|' "$res/values/strings.xml"
  grep -q "(test)" "$res/values/strings.xml" || { echo "could not rename the test app" >&2; exit 1; }
  rename=(--rename-manifest-package "$test_package"
          --rename-instrumentation-target-package "$test_package")
  apk="$out/astro-vr-host-test.apk"
  echo "   as $test_package"
fi
"$BUILD_TOOLS/aapt2.exe" compile --dir "$(cygpath -m "$res")" -o "$(cygpath -m "$out/res.zip")"
"$BUILD_TOOLS/aapt2.exe" link -o "$(cygpath -m "$out/base.apk")" \
  -I "$(cygpath -m "$PLATFORM_JAR")" --manifest "$(cygpath -m "$manifest")" \
  -A "$(cygpath -m "$out/apk/assets")" --debug-mode "${rename[@]}" \
  --min-sdk-version 32 --target-sdk-version 32 "$(cygpath -m "$out/res.zip")"
"$PYTHON" - "$out/base.apk" "$out/apk" <<'PY'
import os, sys, zipfile
apk, staging = sys.argv[1:3]
with zipfile.ZipFile(apk, "a", zipfile.ZIP_DEFLATED) as archive:
    archive.write(os.path.join(staging, "classes.dex"), "classes.dex")
    libs = os.path.join(staging, "lib", "arm64-v8a")
    for name in sorted(os.listdir(libs)):
        archive.write(os.path.join(libs, name), "lib/arm64-v8a/" + name)
PY
"$BUILD_TOOLS/zipalign.exe" -p -f 4 "$(cygpath -m "$out/base.apk")" "$(cygpath -m "$out/aligned.apk")"
# An update only installs over an app signed with the same key. Releases are signed with the
# key named in tools/signing.local (two lines: the keystore, then a file holding its password;
# neither belongs in the repository), other builds with the Android SDK's debug key.
if [[ -f "$root/tools/signing.local" && -z "$test_package" ]]; then
  { read -r keystore; read -r password_file; } < <(tr -d '\r' < "$root/tools/signing.local")
  # (Its key has the keystore's password: apksigner then needs no --key-pass.)
  signing=(--ks "$keystore" --ks-pass "file:$password_file")
  echo "   signed with $(basename "$keystore")"
else
  signing=(--ks "$(cygpath -m "$HOME/.android/debug.keystore")" --ks-pass pass:android
           --key-pass pass:android)
  echo "   signed with the debug key"
fi
java -jar "$(cygpath -m "$BUILD_TOOLS/lib/apksigner.jar")" sign "${signing[@]}" \
  --out "$(cygpath -m "$apk")" "$(cygpath -m "$out/aligned.apk")"

ls -la "$apk"
