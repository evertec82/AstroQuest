from pathlib import Path
import hashlib, io, subprocess, zipfile, shutil, sys, os

root = Path(__file__).resolve().parents[1]
deps = Path(os.environ.get('ASTRO_ANDROID_DEPS', str(root.parent / 'quest-build-deps')))
host = root / 'quest-host'
pico = '--pico' in sys.argv
galaxy = '--galaxy' in sys.argv
assert not (pico and galaxy), 'Choose one platform'
variant = 'galaxy' if galaxy else ('pico' if pico else 'quest')
package = 'com.astrobotquest.vrhost.evertec82' + ('.galaxyxr' if galaxy else ('.pico' if pico else ''))
out = root / ('build/' + variant + '-preview')
out.mkdir(parents=True, exist_ok=True)
stage = out / 'stage'
libs = stage / 'lib/arm64-v8a'
assets = stage / 'assets'
libs.mkdir(parents=True, exist_ok=True)
assets.mkdir(parents=True, exist_ok=True)
ndk = deps / 'android/android-ndk-r27c/toolchains/llvm/prebuilt/windows-x86_64'
bt = deps / 'android-build-tools/android-16'
platform = deps / 'android-platform/android-36/android.jar'
jdk = next((deps / 'jdk').glob('*/bin'))
reference = deps / 'AstroQuest-0.21.1-Quest3.apk'
assert hashlib.sha256(reference.read_bytes()).hexdigest() == '892201225d4179949f2159731762263716ecca17b2b3c3349a2deeeb94022390'
def run(args):
    subprocess.run([str(a) for a in args], check=True)

with zipfile.ZipFile(reference) as z:
    for name in ['libastro_ld.so', 'libopenxr_loader.so']:
        (libs / name).write_bytes(z.read('lib/arm64-v8a/' + name))

if '--package-only' not in sys.argv:
    print('Building Android OpenXR host', flush=True)
    run([ndk/'bin/clang++.exe', '--target=aarch64-linux-android32', '-std=c++20', '-O2',
         *(['-DASTRO_PICO_CPU_FRAMES=1'] if '--cpu-frames' in sys.argv else []), '-fPIC', '-fvisibility=hidden', '-Wall', '-Wextra', '-Wno-unused-parameter',
         '-Wno-missing-field-initializers', '-I', root/'shadps4-arm64-main/externals/openxr-sdk/include',
         '-I', root/'shadps4-arm64-main/src/core/vr', '-I', root/'shadps4-arm64-main/src/input',
         *[host/'cpp'/name for name in ['main.cpp','core_process.cpp','xr_host.cpp','gl_frames.cpp',
                                      'self_test.cpp','log.cpp','xr_probe.cpp']],
         '-shared', '-Wl,-z,max-page-size=16384', '-static-libstdc++', '-Wl,--no-undefined', '-Wl,-soname,libastrovr.so',
         '-L', libs, '-lopenxr_loader', '-lEGL', '-lGLESv3', '-landroid', '-lnativewindow',
         '-laaudio', '-llog', '-lz', '-o', libs/'libastrovr.so'])
    classes = out/'classes'
    dex = out/'dex'
    classes.mkdir(exist_ok=True)
    dex.mkdir(exist_ok=True)
    run([jdk/'javac.exe', '-Xlint:-options', '-source','11','-target','11','-classpath',platform,
         '-d',classes, *sorted((host/'java').rglob('*.java'))])
    run([jdk/'java.exe','-cp',bt/'lib/d8.jar','com.android.tools.r8.D8','--lib',platform,
         '--min-api','32','--output',dex,*sorted(classes.rglob('*.class'))])
    shutil.copy2(dex/'classes.dex',stage/'classes.dex')
    if '--host-only' in sys.argv:
        print('Native host and Java build passed', flush=True)
        sys.exit(0)

core = root/'build/arm64/shadps4'
stripped = out/'shadps4-arm64-fex'
run([deps.parent/'toolchain/clang+llvm-21.1.8-x86_64-pc-windows-msvc/bin/llvm-strip.exe',
     '-o', stripped, core])
with zipfile.ZipFile(reference) as z:
    original = zipfile.ZipFile(io.BytesIO(z.read('assets/runtime.zip')))
    digest = hashlib.sha256()
    with zipfile.ZipFile(assets/'runtime.zip','w',zipfile.ZIP_DEFLATED,compresslevel=6) as runtime:
        for name in sorted(original.namelist()):
            if name.endswith('/'): continue
            data = stripped.read_bytes() if name == 'host/shadps4-arm64-fex' else original.read(name)
            digest.update(name.encode()+b'\0'+data)
            info = zipfile.ZipInfo(name,(2026,1,1,0,0,0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o755 << 16
            runtime.writestr(info,data)
    assert 'host/shadps4-arm64-fex' in original.namelist()
    (assets/'runtime.stamp').write_text(digest.hexdigest()+'\n')
run([bt/'aapt2.exe','compile','--dir',host/'res','-o',out/'res.zip'])
run([bt/'aapt2.exe','link','-o',out/'base.apk','-I',platform,'--manifest',host/('AndroidManifest-galaxy.xml' if galaxy else ('AndroidManifest-pico.xml' if pico else 'AndroidManifest.xml')),
     '-A',assets,'--debug-mode','--rename-manifest-package',package,
     '--rename-instrumentation-target-package',package,
     '--min-sdk-version','32','--target-sdk-version','32',out/'res.zip'])
with zipfile.ZipFile(out/'base.apk','a',zipfile.ZIP_DEFLATED) as z:
    for p in [stage/'classes.dex',*sorted(libs.glob('*.so'))]:
        z.write(p,p.relative_to(stage).as_posix())
run([bt/'zipalign.exe','-p','-f','4',out/'base.apk',out/'aligned.apk'])
key = deps/'quest-preview.keystore'
if not key.exists():
    run([jdk/'keytool.exe','-genkeypair','-keystore',key,'-storepass','android','-keypass','android',
         '-alias','quest-preview','-keyalg','RSA','-keysize','2048','-validity','10000',
         '-dname','CN=AstroQuest Local Preview'])
apk = out/('AstroQuest-GalaxyXR-0.8.7-preview.apk' if galaxy else ('AstroQuest-PICO-0.8.7-preview.apk' if pico else 'AstroQuest-Quest-0.8.7-preview.apk'))
run([jdk/'java.exe','-jar',bt/'lib/apksigner.jar','sign','--ks',key,'--ks-pass','pass:android',
     '--key-pass','pass:android','--out',apk,out/'aligned.apk'])
run([jdk/'java.exe','-jar',bt/'lib/apksigner.jar','verify','--verbose',apk])
run([bt/'aapt2.exe','dump','badging',apk])
print('APK:',apk, '\nSHA256:',hashlib.sha256(apk.read_bytes()).hexdigest(),flush=True)
