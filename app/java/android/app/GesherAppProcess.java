/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.app;

import android.content.Intent;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.List;

/**
 * An app process the way Android runs one, attached to a system server
 * ({@code gesher-app --system-server}): {@code ActivityThread.main}'s steps after the
 * zygote's, so that the server drives the app with the framework's own client path.
 *
 * <ol>
 * <li>A binder thread, as the zygote's child starts its pool, for the server's calls
 *     into the app ({@code IApplicationThread}).
 * <li>{@code new ActivityThread()} and {@code attach(false, startSeq)}:
 *     {@code IActivityManager.attachApplication(mAppThread)}. The server answers with
 *     {@code bindApplication} (→ {@code handleBindApplication}: the LoadedApk, its class
 *     loader, the Application and its {@code onCreate}), and once the app reports
 *     {@code finishAttachApplication}, with a {@code ClientTransaction} of
 *     {@code LaunchActivityItem} and {@code ResumeActivityItem} (→
 *     {@code handleLaunchActivity}, {@code handleStartActivity},
 *     {@code handleResumeActivity}).
 * <li>{@code Looper.loop()} on the main thread, until the server tells the process to
 *     exit ({@code scheduleExit}, → {@code EXIT_APPLICATION}).
 * </ol>
 *
 * <p>The launcher's part, {@code am start -W} then the user removing the task, runs on a
 * thread of its own: {@code IActivityTaskManager.startActivityAndWait} with the launch
 * intent, then (unless the activity finished itself) {@code removeTask}, which makes the
 * server pause, stop and destroy the activity and then end the process.
 *
 * <p>{@link #main} is the entry of a process the system server starts for a package (its
 * zygote, lite: {@code com.gesher.server.Zygote}): {@code --package P --start-seq N
 * --target-sdk S}. It attaches with the start sequence number, which pairs it with the
 * package, and has no launcher part: the server drives it, and tells it when to exit.
 *
 * <p>Windows (docs/graphics.md, ADR 0029): the process draws its own windows. In window
 * mode ({@code $GESHER_APP_WINDOW}: {@code window}, the default, or {@code hidden}) the
 * window session is {@link com.gesher.app.window.WindowSession}, in this process:
 * {@code addToDisplay}/{@code relayout} give each window a Win32 window this process
 * creates and a SurfaceControl whose Surface draws into it, and the hardware renderer
 * (hwui's CPU pipeline) draws the views. Closing the Win32 window finishes its Activity
 * ({@code Activity.finish()}: the system server pauses, stops and destroys it, then ends
 * the process). {@code headless}: the system server's window session (no surface) and no
 * renderer.
 */
public final class GesherAppProcess {
    private static final String TAG = "gesher-app";

    private GesherAppProcess() {
    }

    private static void log(String message) {
        System.out.println(TAG + ": " + message);
        System.out.flush();
    }

    /** Thrown out of Looper.loop() when the main Looper ends the process (scheduleExit). */
    private static volatile boolean sExitRequested;

    /**
     * Runs the app process: attaches it, starts {@code launch} from the launcher thread,
     * loops until the server ends the process. Returns normally when it exited as asked;
     * throws whatever the main thread or the launcher threw.
     *
     * @param targetSdkVersion the app's target SDK, for the runtime (as the zygote sets it)
     * @param removeTaskWhenIdle remove the activity's task once it is idle (the launcher
     *     shuts down), so that the run ends; otherwise the process runs until the
     *     activity finishes itself
     */
    public static void run(Intent launch, int targetSdkVersion, boolean removeTaskWhenIdle)
            throws Exception {
        run(launch, targetSdkVersion, removeTaskWhenIdle,
                com.gesher.app.window.WindowMode.HEADLESS);
    }

    /**
     * {@link #run(Intent, int, boolean)} with the process's windows in {@code windowMode}
     * ({@link com.gesher.app.window.WindowMode#WINDOW}, {@code HIDDEN} or {@code HEADLESS}).
     */
    public static void run(Intent launch, int targetSdkVersion, boolean removeTaskWhenIdle,
            String windowMode) throws Exception {
        run(launch, targetSdkVersion, removeTaskWhenIdle, 0L, windowMode);
    }

    /**
     * {@code android.app.GesherAppProcess --package P --start-seq N [--target-sdk S]}: an app
     * process the system server started for package P (ActivityThread.main after the
     * zygote's fork). Exits with 0 when the server ends it, 1 on a failure.
     */
    public static void main(String[] args) {
        int code = 0;
        try {
            String packageName = null;
            long startSeq = 0;
            int targetSdk = 0;
            for (int i = 0; i + 1 < args.length; i += 2) {
                switch (args[i]) {
                    case "--package": packageName = args[i + 1]; break;
                    case "--start-seq": startSeq = Long.parseLong(args[i + 1]); break;
                    case "--target-sdk": targetSdk = Integer.parseInt(args[i + 1]); break;
                    default: throw new IllegalArgumentException("unknown option " + args[i]);
                }
            }
            if (packageName == null || startSeq <= 0) {
                throw new IllegalArgumentException("usage: GesherAppProcess --package P"
                        + " --start-seq N [--target-sdk S]");
            }
            log("app process for " + packageName + ": pid " + android.os.Process.myPid()
                    + ", start seq " + startSeq);
            String trace = System.getenv("GESHER_APP_METHOD_TRACE");
            if (trace != null && !trace.isEmpty()) {
                // Debugging: ART's method trace of the whole process, streamed (it survives an
                // abort), to <dir>/<package>-<pid>.trace.
                java.io.FileOutputStream out = new java.io.FileOutputStream(trace + "/"
                        + packageName + "-" + android.os.Process.myPid() + ".trace");
                dalvik.system.VMDebug.startMethodTracing("app", out.getFD(), 8 << 20, 0, false,
                        0, true);
            }
            String androidRoot = System.getenv("ANDROID_ROOT");
            if (androidRoot == null || androidRoot.isEmpty()) androidRoot = "/system";
            com.gesher.app.Main.bootstrap(TAG, androidRoot + "/build.prop",
                    androidRoot + "/framework/framework-res.apk");
            // Its windows group under the package's Start menu shortcut on the taskbar
            // ($GESHER_APP_USER_MODEL_ID, from the system server; docs/installer.md).
            com.gesher.app.shell.AppShell.applyProcessAppUserModelId(TAG);
            if (System.getenv("GESHER_APP_STRICT_SERVICES") != null) {
                com.gesher.server.StandIn.setStrict(true);
            }
            // The system server gone, so is the device: this process too (on a device the
            // zygote's children die with system_server).
            android.os.IBinder am = android.os.ServiceManager.getService("activity");
            if (am != null) {
                am.linkToDeath(() -> {
                    System.err.println(TAG + ": the system server died; exiting");
                    System.exit(3);
                }, 0);
            }
            run(null, targetSdk, false, startSeq, com.gesher.app.window.WindowMode
                    .fromEnvironment(com.gesher.app.window.WindowMode.WINDOW));
        } catch (Throwable t) {
            System.out.flush();
            System.err.println(TAG + ": failed: " + t);
            t.printStackTrace();
            code = 1;
        }
        String methodTrace = System.getenv("GESHER_APP_METHOD_TRACE");
        if (methodTrace != null && !methodTrace.isEmpty()) {
            // The trace's last buffer, with how the process got here (docs/e2e.md).
            try {
                dalvik.system.VMDebug.stopMethodTracing();
            } catch (Throwable ignored) {
                // not tracing
            }
        }
        System.out.flush();
        System.err.flush();
        System.exit(code);
    }

    private static void run(Intent launch, int targetSdkVersion, boolean removeTaskWhenIdle,
            long startSeq, String windowMode) throws Exception {
        // The zygote sets the runtime's target SDK in the child before it runs the app
        // (ZygoteConnection: VMRuntime.setTargetSdkVersion): ART's rules for apps (such as
        // which static final fields JNI may still overwrite, as Typeface's font map
        // does) follow it.
        dalvik.system.VMRuntime.getRuntime().setTargetSdkVersion(targetSdkVersion);
        // The zygote's child starts its binder thread pool (ZygoteInit.nativeZygoteInit);
        // here two Java threads join it, before the server can call in.
        com.gesher.server.SystemServer.startBinderThreads("app", 2);
        // Cameras are served in the app's own process (docs/devices.md): media.camera.
        com.gesher.server.CameraService.installInProcess();

        withoutSystemFonts();
        // No vsync source yet (DisplayEventReceiver's natives are stand-ins that never
        // deliver one): Choreographer times its frames with messages instead, as with
        // debug.choreographer.vsync=false. Without frames, the sync barrier a window's
        // first traversal posts would hold every lifecycle message back.
        android.os.SystemProperties.set("debug.choreographer.vsync", "false");
        if (!com.gesher.app.window.WindowMode.HEADLESS.equals(windowMode)) {
            // The process's windows are Win32 windows it creates and draws itself: the window
            // session is in this process (WindowManagerGlobal's), and its surfaces take the
            // hardware renderer's frames (ADR 0029).
            com.gesher.app.window.WindowMode.install(
                    com.gesher.app.window.WindowMode.WINDOW.equals(windowMode));
            log("windows: Win32 windows of this process (" + windowMode + "), display "
                    + com.gesher.app.DeviceProfile.describe());
        } else {
            // Headless: the system server's window session gives windows no surface
            // (StandInWindowSession), so they are laid out but not drawn and get no hardware
            // renderer, as ActivityThread leaves persistent processes on low-end devices
            // without one (with no surface the render thread faulted in CanvasContext,
            // integration ci run 40003812869).
            android.view.ThreadedRenderer.sRendererEnabled = false;
        }

        // ActivityThread.main: the main Looper is prepared (Main.bootstrap), then
        // new ActivityThread().attach(false, startSeq).
        ActivityThread thread = new ActivityThread();
        Method attach = ActivityThread.class.getDeclaredMethod("attach", boolean.class,
                long.class);
        attach.setAccessible(true);
        if (ActivityThread.sMainThreadHandler == null) {
            ActivityThread.sMainThreadHandler = thread.getHandler();
        }
        watchForExit(thread.mH);

        try {
            attach.invoke(thread, false, startSeq);
        } catch (java.lang.reflect.InvocationTargetException e) {
            throw rethrow(e.getCause());
        }
        log("attached to the system server (pid " + android.os.Process.myPid() + ")");

        // The launcher asks for the start once the process is attached: the server gives
        // the start to the process that asked for it, rather than starting another one.
        final Throwable[] launcherFailure = new Throwable[1];
        if (launch != null) {
            Thread launcher = new Thread(() -> {
                try {
                    launch(launch, removeTaskWhenIdle);
                } catch (Throwable t) {
                    launcherFailure[0] = t;
                    System.err.println(TAG + ": the launcher failed: " + t);
                    t.printStackTrace();
                    // Nothing will end the process now: end it from its main thread.
                    new android.os.Handler(Looper.getMainLooper()).post(() -> {
                        throw new IllegalStateException("the launcher failed", t);
                    });
                }
            }, "launcher");
            launcher.setDaemon(true);
            launcher.start();
        }

        if (System.getenv("GESHER_APP_TRACE_LOOPER") != null) {
            // Debugging: each message the main Looper dispatches, on stderr.
            Looper.getMainLooper().setMessageLogging(
                    line -> System.err.println("looper: " + line));
        }
        try {
            Looper.loop();
        } catch (IllegalStateException e) {
            // EXIT_APPLICATION: Application.onTerminate, then Looper.quit(), which the main
            // Looper refuses with this exception; on a device the server kills the process.
            if (!sExitRequested) throw e;
        }
        log("the system server ended the process (scheduleExit)");
        if (launcherFailure[0] != null) throw rethrow(launcherFailure[0]);
    }

    private static Exception rethrow(Throwable t) {
        if (t instanceof Error) throw (Error) t;
        if (t instanceof Exception) return (Exception) t;
        return new RuntimeException(t);
    }

    /** The launcher's part: am start -W, then remove the task. */
    private static void launch(Intent launch, boolean removeTaskWhenIdle) throws Exception {
        IActivityTaskManager atm = ActivityTaskManager.getService();
        WaitResult result = atm.startActivityAndWait(null /* caller */, "com.gesher.app",
                null /* callingFeatureId */, launch, null /* resolvedType */,
                null /* resultTo */, null /* resultWho */, 0 /* requestCode */,
                0 /* flags */, null /* profilerInfo */, null /* options */, 0 /* userId */);
        log("startActivityAndWait: result " + result.result + ", "
                + (result.who != null ? result.who.flattenToShortString() : "no activity")
                + (result.timeout ? ", timed out" : "") + ", " + result.totalTime + " ms");
        if (result.result != ActivityManager.START_SUCCESS) {
            throw new IllegalStateException("the system server did not start " + launch);
        }
        if (!removeTaskWhenIdle) return;
        ActivityManager.RunningTaskInfo task = taskOf(atm, result.who.getPackageName());
        if (task == null) return;  // the activity finished by itself
        log("removing task " + task.taskId + " (" + task.baseActivity.flattenToShortString()
                + ")");
        atm.removeTask(task.taskId);
    }

    /** The most recent task whose base activity is the package's, or null. */
    public static ActivityManager.RunningTaskInfo taskOf(IActivityTaskManager atm,
            String packageName) throws android.os.RemoteException {
        for (ActivityManager.RunningTaskInfo t : atm.getTasks(Integer.MAX_VALUE,
                false /* filterOnlyVisibleRecents */, false /* keepIntentExtra */,
                android.view.Display.INVALID_DISPLAY)) {
            if (t.baseActivity != null && t.baseActivity.getPackageName().equals(packageName)) {
                return t;
            }
        }
        return null;
    }

    /**
     * WORKAROUND for a Gesher without system fonts: with no fonts.xml,
     * handleBindApplication's {@code Typeface.setSystemFontMap} gets an empty font map and
     * no default typeface, and fails. Here, as when a font map is loaded, the default
     * family ({@code sans-serif}) gets a typeface first: one made of no font family.
     * Nothing is drawn with it. When gesher-app has loaded the installed fonts
     * ($ANDROID_ROOT/etc/fonts.xml, com.gesher.app.fonts.SystemFontMap), it does nothing.
     */
    private static void withoutSystemFonts() throws ReflectiveOperationException {
        // A process the system server started (main) has not loaded the installed fonts
        // the way gesher-app's Main does; load them here first.
        try {
            String fonts = com.gesher.app.fonts.SystemFontMap.load();
            if (fonts != null) {
                log("system fonts: " + fonts);
            }
        } catch (Exception e) {
            log("system fonts not loaded: " + e);
        }
        if (new java.io.File(android.graphics.fonts.SystemFonts.LEGACY_FONTS_XML).exists()
                || android.graphics.Typeface.getSystemFontMap().containsKey("sans-serif")) {
            return;
        }
        java.lang.reflect.Method create = android.graphics.Typeface.class.getDeclaredMethod(
                "nativeCreateFromArray", long[].class, long.class, int.class, int.class);
        create.setAccessible(true);
        long ni = (long) create.invoke(null, new long[0], 0L,
                android.graphics.Typeface.RESOLVE_BY_FONT_TABLE,
                android.graphics.Typeface.RESOLVE_BY_FONT_TABLE);
        java.lang.reflect.Constructor<android.graphics.Typeface> c =
                android.graphics.Typeface.class.getDeclaredConstructor(long.class,
                        String.class);
        c.setAccessible(true);
        android.graphics.Typeface.setSystemFontMap(
                java.util.Map.of("sans-serif", c.newInstance(ni, "sans-serif")));
        log("no system fonts (" + android.graphics.fonts.SystemFonts.LEGACY_FONTS_XML
                + "): an empty sans-serif typeface stands in");
    }

    /** Notes EXIT_APPLICATION on its way to ActivityThread's H. */
    private static void watchForExit(Handler h) throws ReflectiveOperationException {
        Field callback = Handler.class.getDeclaredField("mCallback");
        callback.setAccessible(true);
        callback.set(h, (Handler.Callback) (Message msg) -> {
            if (msg.what == ActivityThread.H.EXIT_APPLICATION) {
                sExitRequested = true;
            }
            return false;  // ActivityThread handles it
        });
    }
}
