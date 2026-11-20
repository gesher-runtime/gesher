/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.app;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.PrintWriter;
import java.io.Writer;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;

/** Append-only event records keep an older open notice's evidence identifiable. */
public final class CrashReport {
    private CrashReport() {}

    public static String append(File file, String pkg, int pid, Throwable failure) throws IOException {
        String event = Integer.toHexString(pid) + "-" + Long.toHexString(System.nanoTime());
        BoundedText stack = new BoundedText();
        failure.printStackTrace(new PrintWriter(stack));
        String record = "\n>>> Gesher crash event " + event + " package " + pkg + " at "
                + System.currentTimeMillis() + "\n" + stack.text
                + (stack.truncated ? "\n[stack trace truncated]\n" : "")
                + "<<< End crash event " + event + "\n";
        file.getParentFile().mkdirs();
        try (FileOutputStream out = new FileOutputStream(file, true);
                FileLock lock = out.getChannel().tryLock()) {
            // Never wait behind another dying process. If writing is unavailable,
            // the caller keeps its original logs and failure/exit behavior.
            if (lock == null) throw new IOException("crash report is busy");
            out.write(record.getBytes(StandardCharsets.UTF_8));
        }
        return event;
    }

    private static final class BoundedText extends Writer {
        final StringBuilder text = new StringBuilder();
        boolean truncated;
        @Override public void write(char[] chars, int off, int len) {
            int take = Math.min(len, 128 * 1024 - text.length());
            text.append(chars, off, take);
            truncated |= take < len;
        }
        @Override public void flush() {}
        @Override public void close() {}
    }
}
