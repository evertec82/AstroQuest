// SPDX-License-Identifier: GPL-2.0-or-later
package com.astrobotquest.vrhost;

import android.app.Activity;
import android.app.Application;
import android.app.Instrumentation;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

/**
 * Development aid: runs things inside the app's sandbox without any activity, which means
 * without anybody wearing (and unlocking) the headset.
 *
 * "adb shell" and "run-as" are not what the emulator core meets when the app starts it: they run
 * under different SELinux domains (on Quest neither may allocate GPU memory) and without the
 * system-call filter apps get. This runs as a real app process instead.
 *
 * A shell command, in the app's files folder; the tail of its output comes back as the result:
 *
 *   adb shell am instrument -w -e cmd "<command>" com.astrobotquest.vrhost/.SandboxShell
 *
 * The host's own code path, minus the headset session: the core is started the way the app
 * starts it, gets a controller and a moving head over the app's sockets, renders into the app's
 * shared frame buffers, and what arrives there is saved as pictures in files/selftest:
 *
 *   adb shell am instrument -w -e selftest <seconds> [-e game <eboot.bin>] [-e env "A=1 B=2"] \
 *       [-e mic test|real|off] [-e install 1] com.astrobotquest.vrhost/.SandboxShell
 *
 * The game's microphone hears a test signal there (a second of noise every five), unless "mic"
 * says to open the headset's own or none.
 *
 * The app's own start, without showing anything: the activity is created and its onCreate runs
 * as it does when the app is launched (settings, OpenXR, runtime, emulator, status panel), for
 * the given seconds; then it is destroyed. What comes back is the host.log it wrote:
 *
 *   adb shell am instrument -w -e drystart <seconds> [-e settings <file like vrhost.txt>] \
 *       com.astrobotquest.vrhost/.SandboxShell
 *
 * What the headset's OpenXR runtime says of itself without a session ("1": that the app finds
 * it and which of the extensions the host uses it has; "2": also what an instance tells, should
 * the runtime let one be made without an activity; "3": the host itself, for the five seconds
 * it takes to set up everything that comes before a session). The log is files/xrprobe.log:
 *
 *   adb shell am instrument -w -e xrprobe 1 com.astrobotquest.vrhost/.SandboxShell
 */
public class SandboxShell extends Instrumentation {
    private static final String TAG = "AstroVR";
    private static final int MAX_OUTPUT = 60 * 1024;

    private Bundle arguments;

    private static native String nativeSelfTest(String loader, String runtimeRoot,
            String storageRoot, String game, String logFile, String outDir, int seconds,
            String[] extraEnv, String microphone);

    private static native String nativeXrProbe(Object context, String logFile, int level);

    @Override
    public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        this.arguments = arguments;
        start();
    }

    @Override
    public void onStart() {
        Bundle result = new Bundle();
        try {
            String selfTest = arguments.getString("selftest");
            String xrProbe = arguments.getString("xrprobe");
            String dryStart = arguments.getString("drystart");
            String report;
            if (dryStart != null) {
                report = runDryStart(Integer.parseInt(dryStart));
            } else if (xrProbe != null) {
                System.loadLibrary("astrovr");
                report = nativeXrProbe(getTargetContext(),
                        new File(getTargetContext().getFilesDir(), "xrprobe.log")
                                .getAbsolutePath(),
                        Integer.parseInt(xrProbe));
            } else {
                report = selfTest != null ? runSelfTest(Integer.parseInt(selfTest)) : runCommand();
            }
            result.putString(REPORT_KEY_STREAMRESULT, report);
        } catch (Throwable e) {
            Log.e(TAG, "sandbox run failed", e);
            result.putString(REPORT_KEY_STREAMRESULT, "failed: " + e + "\n");
        }
        finish(Activity.RESULT_OK, result);
    }

    /**
     * Creates the app's activity the way the system does, short of giving it a window on a
     * display, and lets it start everything it starts.
     */
    private String runDryStart(int seconds) throws Exception {
        // Where the activity leaves the pictures of its status panel, emptied of the last run's.
        File pictures = new File(getTargetContext().getFilesDir(), "drystart");
        RuntimeInstaller.deleteRecursively(pictures);
        pictures.mkdirs();
        // (An earlier version of this test put them next to the logs.)
        for (int number = 0; number < 8; ++number) {
            new File(getTargetContext().getExternalFilesDir(null), "status-" + number + ".png")
                    .delete();
        }

        final Intent intent = new Intent(getTargetContext(), MainActivity.class);
        intent.putExtra("dry_run", true);
        if (arguments.getString("settings") != null) {
            intent.putExtra("settings", arguments.getString("settings"));
        }
        final ActivityInfo info = getTargetContext().getPackageManager().getActivityInfo(
                new ComponentName(getTargetContext(), MainActivity.class), 0);
        final Activity[] activity = new Activity[1];
        final Throwable[] failure = new Throwable[1];
        runOnMainSync(() -> {
            try {
                activity[0] = newActivity(MainActivity.class, getTargetContext(), null,
                        (Application) getTargetContext().getApplicationContext(), intent, info,
                        "Astro VR Host", null, null, null);
                callActivityOnCreate(activity[0], null);
            } catch (Throwable e) {
                failure[0] = e;
            }
        });
        StringBuilder report = new StringBuilder();
        if (failure[0] != null) {
            Log.e(TAG, "dry start failed", failure[0]);
            report.append("the activity failed to start: ").append(Log.getStackTraceString(failure[0]))
                    .append('\n');
        } else {
            // A controller that is not there: the events a gamepad would cause are handed to
            // the activity as the system hands them over. Cross for a quarter of every five
            // seconds gets the game past its first screens, the stick is moved in between.
            long end = System.currentTimeMillis() + seconds * 1000L;
            for (int step = 0; System.currentTimeMillis() < end; ++step) {
                Thread.sleep(250);
                final int phase = step % 20;
                // Once, OPTIONS held for a second and a half and then the PS button: both ask
                // for the view to be reset.
                final int reset = step == 104 ? KeyEvent.KEYCODE_BUTTON_START
                        : step == 110 ? -KeyEvent.KEYCODE_BUTTON_START
                        : step == 114 ? KeyEvent.KEYCODE_BUTTON_MODE
                        : step == 115 ? -KeyEvent.KEYCODE_BUTTON_MODE : 0;
                // Once, what a gamepad without a touchpad does about the touchpad: the right
                // stick pulled back for most of a second and let go (the finger it moves),
                // and R1 (which swipes forward). With env=SHADPS4_PAD_TOUCH_TRACE=1 among the
                // settings, core.log says what the game was given of both.
                final int gesture = step == 122 ? 1 : step == 125 ? 2
                        : step == 128 ? KeyEvent.KEYCODE_BUTTON_R1
                        : step == 129 ? -KeyEvent.KEYCODE_BUTTON_R1 : 0;
                if (reset == 0 && gesture == 0
                        && (step < 80 || (phase != 0 && phase != 1 && phase != 10))) {
                    continue;
                }
                runOnMainSync(() -> {
                    try {
                        if (reset != 0) {
                            activity[0].dispatchKeyEvent(new KeyEvent(
                                    reset > 0 ? KeyEvent.ACTION_DOWN : KeyEvent.ACTION_UP,
                                    Math.abs(reset)));
                        } else if (gesture == 1 || gesture == 2) {
                            activity[0].dispatchGenericMotionEvent(
                                    stickEvent(0.0f, 0.0f, 0.0f, gesture == 1 ? 1.0f : 0.0f));
                        } else if (gesture != 0) {
                            activity[0].dispatchKeyEvent(new KeyEvent(
                                    gesture > 0 ? KeyEvent.ACTION_DOWN : KeyEvent.ACTION_UP,
                                    Math.abs(gesture)));
                        } else if (phase == 10) {
                            activity[0].dispatchGenericMotionEvent(
                                    stickEvent(0.5f, -0.25f, 0.0f, 0.0f));
                        } else {
                            activity[0].dispatchKeyEvent(new KeyEvent(
                                    phase == 0 ? KeyEvent.ACTION_DOWN : KeyEvent.ACTION_UP,
                                    KeyEvent.KEYCODE_BUTTON_A));
                        }
                    } catch (Throwable e) {
                        failure[0] = e;
                    }
                });
                if (failure[0] != null) {
                    report.append("input failed: ").append(Log.getStackTraceString(failure[0]))
                            .append('\n');
                    failure[0] = null;
                    break;
                }
            }
        }
        if (activity[0] != null) {
            runOnMainSync(() -> {
                try {
                    callActivityOnDestroy(activity[0]);
                } catch (Throwable e) {
                    failure[0] = e;
                }
            });
            if (failure[0] != null) {
                report.append("stopping failed: ").append(Log.getStackTraceString(failure[0]))
                        .append('\n');
            }
        }
        File log = new File(getTargetContext().getExternalFilesDir(null), "host.log");
        if (log.isFile()) {
            report.append(new String(Files.readAllBytes(log.toPath()), StandardCharsets.UTF_8));
        } else {
            report.append("no host.log was written\n");
        }
        return report.toString();
    }

    /** What a gamepad reports when its sticks are moved: the left one, the right one. */
    private static MotionEvent stickEvent(float x, float y, float rightX, float rightY) {
        MotionEvent.PointerProperties[] properties = {new MotionEvent.PointerProperties()};
        properties[0].id = 0;
        MotionEvent.PointerCoords[] coords = {new MotionEvent.PointerCoords()};
        coords[0].setAxisValue(MotionEvent.AXIS_X, x);
        coords[0].setAxisValue(MotionEvent.AXIS_Y, y);
        coords[0].setAxisValue(MotionEvent.AXIS_Z, rightX);
        coords[0].setAxisValue(MotionEvent.AXIS_RZ, rightY);
        long now = SystemClock.uptimeMillis();
        return MotionEvent.obtain(now, now, MotionEvent.ACTION_MOVE, 1, properties, coords, 0, 0,
                1.0f, 1.0f, 0, 0, InputDevice.SOURCE_JOYSTICK, 0);
    }

    private String runCommand() throws Exception {
        String command = arguments.getString("cmd", "id");
        File directory = getTargetContext().getFilesDir();
        directory.mkdirs();
        ProcessBuilder builder = new ProcessBuilder("/system/bin/sh", "-c", command);
        builder.directory(directory);
        builder.redirectErrorStream(true);
        builder.environment().put("ASTRO_LIB_DIR",
                getTargetContext().getApplicationInfo().nativeLibraryDir);
        builder.environment().put("ASTRO_EXTERNAL_DIR",
                String.valueOf(getTargetContext().getExternalFilesDir(null)));
        Process process = builder.start();

        // Keep the end of the output, that is where failures are.
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[4096];
        try (InputStream in = process.getInputStream()) {
            for (int count = in.read(buffer); count > 0; count = in.read(buffer)) {
                output.write(buffer, 0, count);
                if (output.size() > 2 * MAX_OUTPUT) {
                    byte[] all = output.toByteArray();
                    output.reset();
                    output.write(all, all.length - MAX_OUTPUT, MAX_OUTPUT);
                }
            }
        }
        int exit = process.waitFor();
        byte[] all = output.toByteArray();
        int from = Math.max(0, all.length - MAX_OUTPUT);
        return new String(all, from, all.length - from, StandardCharsets.UTF_8)
                + "\nexit=" + exit + "\n";
    }

    private String runSelfTest(int seconds) {
        System.loadLibrary("astrovr");
        File files = getTargetContext().getFilesDir();
        File out = new File(files, "selftest");
        RuntimeInstaller.deleteRecursively(out);
        out.mkdirs();
        File storage = new File(files, "selftest-core");
        storage.mkdirs();

        // "install": the runtime comes out of the app's own assets first, as on the app's first
        // start, instead of being whatever the test scripts last put there.
        String installed = "";
        if (arguments.getString("install") != null) {
            try {
                File runtime = new File(files, "runtime");
                new File(runtime, ".stamp").delete();
                long begin = System.currentTimeMillis();
                RuntimeInstaller.install(getTargetContext(), runtime);
                installed = "runtime unpacked from the app in "
                        + (System.currentTimeMillis() - begin) + " ms\n";
            } catch (java.io.IOException e) {
                return "the runtime could not be unpacked: " + e + "\n";
            }
        }

        List<String> env = new ArrayList<>();
        env.add("XDG_DATA_HOME=" + new File(out, "data"));
        env.add("SHADPS4_MAX_MSAA=1");
        for (String entry : arguments.getString("env", "").split(" ")) {
            if (!entry.isEmpty()) {
                env.add(entry);
            }
        }
        String game = arguments.getString("game", "/data/local/tmp/astro/games/CUSA12392/eboot.bin");
        return installed + nativeSelfTest(
                getTargetContext().getApplicationInfo().nativeLibraryDir + "/libastro_ld.so",
                new File(files, "runtime").getAbsolutePath(), storage.getAbsolutePath(), game,
                new File(out, "core.log").getAbsolutePath(), out.getAbsolutePath(), seconds,
                env.toArray(new String[0]), arguments.getString("mic", "test"));
    }
}
