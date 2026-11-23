/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.app;

import android.app.ActivityThread;
import android.app.Application;
import com.gesher.app.shell.Shell;
import com.gesher.app.window.WindowSession;
import com.gesher.server.Device;
import com.gesher.server.WindowsBluetooth;
import com.gesher.server.Zygote;
import java.util.Collections;

/** Best-effort desktop notice for an app whose main entry point is already failing. */
public final class AppCrash {
    private AppCrash() {}

    public static void report(Throwable failure) {
        try {
            if ("0".equals(System.getenv("GESHER_CRASH_DIALOGS"))) return;
            WindowSession session = WindowSession.get();
            // Background-only processes and hidden/headless automation must stay quiet.
            if (session == null || !session.hasShownApplicationWindow()) return;
            Application app = ActivityThread.currentApplication();
            if (app == null) return;
            String pkg = app.getPackageName();
            String exe = Zygote.exePath();
            String reporter = exe.substring(0, exe.lastIndexOf('\\')) + "\\gesher-launch.exe";
            if (!Shell.exists(reporter)) return;
            String kind = CrashDiagnosis.kind(failure, () -> {
                WindowsBluetooth.Adapter adapter = WindowsBluetooth.cachedAdapter(10_000);
                // An activation/JNI failure is not evidence of missing hardware.
                if (adapter == null || adapter.hr != 0x8007048f) {
                    throw new IllegalStateException("unknown radio state");
                }
                return adapter.present;
            });
            // Standalone/console runs may have redirected their output elsewhere.
            // Give every notice its own text report rather than pointing at a log
            // which may not exist (or at the binary .logcat stream).
            java.io.File report = new java.io.File(Device.logsDir(), pkg + ".crash.log");
            String event = CrashReport.append(report, pkg, android.os.Process.myPid(), failure);
            String log = Zygote.windowsPath(report.getAbsolutePath());
            String command = Zygote.quote(reporter) + " --app-crash " + Zygote.quote(pkg)
                    + " " + Zygote.quote(kind) + " " + Zygote.quote(log) + " " + Zygote.quote(event);
            // A separate host process owns the dialog. Never wait for its dismissal or
            // replace the original exception, exit code, or process cleanup.
            Zygote.spawn(command, Collections.emptyList(),
                    new java.io.File(Device.logsDir(), "crash-dialog.log").getAbsolutePath(), true);
        } catch (Throwable noticeFailure) {
            System.err.println("gesher-app: crash notice unavailable: " + noticeFailure);
        }
    }
}
