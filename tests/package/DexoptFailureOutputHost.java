/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.util.List;

/**
 * Dexopt.failureLines over dex2oat's and profman's real failure output (as dexopt.log
 * holds it): the lines that say why reach system_server.log.
 */
public final class DexoptFailureOutputHost {
    private static int checks;

    private static void check(boolean ok, String message) {
        if (!ok) throw new AssertionError(message);
        checks++;
    }

    public static void main(String[] args) {
        // dex2oat given an APK path it cannot open (a non-ASCII path in the code page).
        String missing = String.join("\n",
                "11-23 02:11:51.918  5612 13760 I dex2oat.exe: dex2oat.cc:3176 dex2oat.exe --dex-file=/x/base.apk",
                "11-23 02:11:51.918  5612 13760 W dex2oat.exe: runtime.cc:1723 Could not reserve sentinel fault page",
                "11-23 02:11:51.918  5612 13760 W dex2oat.exe: dex2oat.cc:2614 Skipping non-existent dex file '/x/base.apk'",
                "11-23 02:11:51.919  5612 13760 E dex2oat.exe: dex2oat.cc:1260 Failed to create oat file: /x/oat/base.odex: No such file or directory",
                "11-23 02:11:51.919  5612 13760 I dex2oat.exe: dex2oat.cc:2904 dex2oat took 1.715ms (0 cpu) (threads: 4)",
                "");
        List<String> lines = Dexopt.failureLines(missing);
        check(lines.size() == 3, "two warnings before the error, then the error: " + lines);
        check(lines.get(1).contains("Skipping non-existent dex file"), "the cause: " + lines);
        check(lines.get(2).contains("Failed to create oat file"), "the error: " + lines);

        // A usage error: the cause first, then the usage text, cut to 8 lines.
        StringBuilder usage = new StringBuilder(
                "11-23 02:04:58.948 14400 21880 E dex2oat.exe: dex2oat.cc:220 --android-root unspecified and ANDROID_ROOT not set\r\n");
        usage.append("11-23 02:04:58.949 14400 21880 E dex2oat.exe: dex2oat.cc:220 Command: ")
                .append("x".repeat(1000)).append("\r\n");
        for (int i = 0; i < 200; i++) {
            usage.append("11-23 02:04:58.949 14400 21880 E dex2oat.exe: dex2oat.cc:220 --option-").append(i).append("\r\n");
        }
        usage.append("See log for usage error information\r\n");
        lines = Dexopt.failureLines(usage.toString());
        check(lines.size() == 8, "at most 8 lines: " + lines.size());
        check(lines.get(0).endsWith("ANDROID_ROOT not set"), "the cause first: " + lines.get(0));
        check(lines.get(1).length() == 403 && lines.get(1).endsWith("..."), "a long line cut: " + lines.get(1).length());

        // profman's I/O error (ProfmanResult 4).
        lines = Dexopt.failureLines(
                "11-23 02:18:46.900 17248 26168 E profman.exe: profman.cc:653 Unable to open '/x/base.apk': No such file or directory\n");
        check(lines.size() == 1 && lines.get(0).contains("Unable to open"), "profman: " + lines);

        // No ART log lines (a crash message): the last lines.
        lines = Dexopt.failureLines("one\ntwo\n\nthree\n");
        check(lines.equals(List.of("one", "two", "three")), "plain output: " + lines);
        check(Dexopt.failureLines("").isEmpty(), "no output");

        System.out.println("DexoptFailureOutputHost: " + checks + " checks passed");
    }
}
