/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.content.Context;
import android.os.Process;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

/** Lines on stdout (the package's log), prefixed "devsvc: ", for the tests to check. */
final class Report {
    private Report() {
    }

    static void line(String message) {
        System.out.println("devsvc: " + message);
        System.out.flush();
        Log.i("devsvc", message);
    }

    /** "... (pid N)": which process ran it. */
    static String pid() {
        return " (pid " + Process.myPid() + ")";
    }

    /** Adds one to the counter {@code name} in the app's files (across its processes). */
    static synchronized int count(Context context, String name) {
        File f = new File(context.getFilesDir(), name + ".count");
        int n = 0;
        try (FileInputStream in = new FileInputStream(f)) {
            n = Integer.parseInt(new String(in.readAllBytes(), StandardCharsets.UTF_8).trim());
        } catch (Exception e) {
            // first time
        }
        n++;
        try (FileOutputStream out = new FileOutputStream(f)) {
            out.write(String.valueOf(n).getBytes(StandardCharsets.UTF_8));
        } catch (Exception e) {
            line("counter " + name + " not written: " + e);
        }
        return n;
    }
}
