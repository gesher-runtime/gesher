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
     * @param removeTaskWhenIdle remove the activity's task once it is idle (the launcher
     *     shuts down), so that the run ends; otherwise the process runs until the
     *     activity finishes itself
     */
    public static void run(Intent launch, boolean removeTaskWhenIdle) throws Exception {
        // The zygote's child starts its binder thread pool (ZygoteInit.nativeZygoteInit);
        // here two Java threads join it, before the server can call in.
        com.gesher.server.SystemServer.startBinderThreads("app", 2);

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

        final Throwable[] launcherFailure = new Throwable[1];
        Thread launcher = new Thread(() -> {
            try {
                launch(launch, removeTaskWhenIdle);
            } catch (Throwable t) {
                launcherFailure[0] = t;
                System.err.println(TAG + ": the launcher failed: " + t);
                t.printStackTrace();
            }
        }, "launcher");
        launcher.setDaemon(true);
        launcher.start();

        try {
            attach.invoke(thread, false, 0L);
        } catch (java.lang.reflect.InvocationTargetException e) {
            throw rethrow(e.getCause());
        }
        log("attached to the system server (pid " + android.os.Process.myPid() + ")");

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
        List<ActivityManager.RunningTaskInfo> tasks = atm.getTasks(1,
                false /* filterOnlyVisibleRecents */, false /* keepIntentExtra */,
                android.view.Display.INVALID_DISPLAY);
        if (tasks.isEmpty()) return;  // the activity finished by itself
        int taskId = tasks.get(0).taskId;
        log("removing task " + taskId + " (" + tasks.get(0).baseActivity.flattenToShortString()
                + ")");
        atm.removeTask(taskId);
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
