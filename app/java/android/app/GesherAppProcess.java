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
        run(launch, targetSdkVersion, removeTaskWhenIdle, 0L);
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
            run(null, targetSdk, false, startSeq);
        } catch (Throwable t) {
            System.out.flush();
            System.err.println(TAG + ": failed: " + t);
            t.printStackTrace();
            code = 1;
        }
        System.out.flush();
        System.err.flush();
        System.exit(code);
    }

    private static void run(Intent launch, int targetSdkVersion, boolean removeTaskWhenIdle,
            long startSeq) throws Exception {
        // The zygote sets the runtime's target SDK in the child before it runs the app
        // (ZygoteConnection: VMRuntime.setTargetSdkVersion): ART's rules for apps (such as
        // which static final fields JNI may still overwrite, as Typeface's font map
        // does) follow it.
        dalvik.system.VMRuntime.getRuntime().setTargetSdkVersion(targetSdkVersion);
        // The zygote's child starts its binder thread pool (ZygoteInit.nativeZygoteInit);
        // here two Java threads join it, before the server can call in.
        com.gesher.server.SystemServer.startBinderThreads("app", 2);

        withoutSystemFonts();
        // No vsync source yet (DisplayEventReceiver's natives are stand-ins that never
        // deliver one): Choreographer times its frames with messages instead, as with
        // debug.choreographer.vsync=false. Without frames, the sync barrier a window's
        // first traversal posts would hold every lifecycle message back.
        android.os.SystemProperties.set("debug.choreographer.vsync", "false");
        // The system server's window session gives windows no surface (StandInWindowSession):
        // the windows are laid out but not drawn, so they get no hardware renderer, as
        // ActivityThread leaves persistent processes on low-end devices without one. With
        // hwui's real natives and no surface, the render thread faulted in CanvasContext
        // (integration ci run 40003812869). GESHER_APP_HWUI=1 keeps the renderer, for a
        // window session that gives a surface.
        if (!"1".equals(System.getenv("GESHER_APP_HWUI"))) {
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
     * WORKAROUND until system fonts are installed (graphics track): with no fonts.xml,
     * handleBindApplication's {@code Typeface.setSystemFontMap} gets an empty font map and
     * no default typeface, and fails. Here, as when a font map is loaded, the default
     * family ({@code sans-serif}) gets a typeface first: one made of no font family (by
     * the Typeface natives, stand-ins until hwui's are linked). Nothing is drawn with it.
     */
    private static void withoutSystemFonts() throws ReflectiveOperationException {
        if (new java.io.File(android.graphics.fonts.SystemFonts.LEGACY_FONTS_XML).exists()) {
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
