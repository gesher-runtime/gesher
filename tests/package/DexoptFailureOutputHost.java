/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.util.List;

/**
 * Dexopt.failureLines over dex2oat's and profman's real failure output (as dexopt.log
 * holds it): the lines that say why a run failed reach system_server.log, and only the
 * failed process's lines.
 */
public final class DexoptFailureOutputHost {
    private static int checks;

    private static void check(boolean ok, String message) {
        if (!ok) throw new AssertionError(message);
        checks++;
    }

    private static boolean anyContains(List<String> lines, String text) {
        for (String l : lines) if (l.contains(text)) return true;
        return false;
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
        List<String> lines = Dexopt.failureLines(missing, 5612);
        check(lines.size() == 2, "the cause warning and the error: " + lines);
        check(lines.get(0).contains("Skipping non-existent dex file"), "the cause first: " + lines);
        check(lines.get(1).contains("Failed to create oat file"), "then the error: " + lines);

        // Two runs appending to dexopt.log at once: background profman (pid 7001) beside an
        // install's dex2oat (pid 5612). Each run's lines are its own.
        String interleaved = String.join("\n",
                "$ \"C:\\Gesher\\profman.exe\" --profile-file=/p/cur.prof --apk=/q/base.apk",
                "11-23 02:11:51.918  5612 13760 W dex2oat.exe: dex2oat.cc:2614 Skipping non-existent dex file '/x/base.apk'",
                "11-23 02:11:51.918  7001  7002 E profman.exe: profman.cc:653 Unable to open '/q/base.apk': No such file or directory",
                "11-23 02:11:51.919  5612 13760 E dex2oat.exe: dex2oat.cc:1260 Failed to create oat file: /x/oat/base.odex",
                "11-23 02:11:51.919  7001  7002 E profman.exe: profile_assistant.cc:112 Could not clear reference profile file",
                "");
        lines = Dexopt.failureLines(interleaved, 5612);
        check(lines.size() == 2 && !anyContains(lines, "profman") && !anyContains(lines, "$ "),
                "dex2oat's lines only: " + lines);
        lines = Dexopt.failureLines(interleaved, 7001);
        check(lines.size() == 2 && !anyContains(lines, "dex2oat"), "profman's lines only: " + lines);
        check(lines.get(0).contains("Unable to open"), "profman's cause: " + lines);
        // A pid whose digits are a prefix of another's is not that one.
        check(Dexopt.failureLines(interleaved, 700).isEmpty(), "pid 700 wrote nothing");

        // A real cause after earlier non-fatal errors: the cause is kept.
        StringBuilder noisy = new StringBuilder();
        for (int i = 0; i < 12; i++) {
            noisy.append("11-23 02:11:51.918  5612 13760 E dex2oat.exe: verifier.cc:1 non-fatal ").append(i).append('\n');
        }
        noisy.append("11-23 02:11:51.919  5612 13760 E dex2oat.exe: dex2oat.cc:1260 Failed to create oat file: /x\n");
        lines = Dexopt.failureLines(noisy.toString(), 5612);
        check(lines.size() == 8, "at most 8 lines: " + lines.size());
        check(lines.get(7).contains("Failed to create oat file"), "the late cause kept, in order: " + lines);
        check(lines.get(0).contains("non-fatal 0"), "then the first errors: " + lines);

        // A usage error: the cause first, then the usage text, cut to 8 lines.
        StringBuilder usage = new StringBuilder(
                "11-23 02:04:58.948 14400 21880 E dex2oat.exe: dex2oat.cc:220 --android-root unspecified and ANDROID_ROOT not set\r\n");
        usage.append("11-23 02:04:58.949 14400 21880 E dex2oat.exe: dex2oat.cc:220 Command: ")
                .append("x".repeat(1000)).append("\r\n");
        for (int i = 0; i < 200; i++) {
            usage.append("11-23 02:04:58.949 14400 21880 E dex2oat.exe: dex2oat.cc:220 --option-").append(i).append("\r\n");
        }
        usage.append("See log for usage error information\r\n");
        lines = Dexopt.failureLines(usage.toString(), 14400);
        check(lines.size() == 8, "at most 8 lines: " + lines.size());
        check(lines.get(0).endsWith("ANDROID_ROOT not set"), "the cause first: " + lines.get(0));
        check(lines.get(1).length() == 403 && lines.get(1).endsWith("..."), "a long line cut: " + lines.get(1).length());

        // No ART lines of the process (a crash message): the last unattributed lines.
        lines = Dexopt.failureLines("$ x.exe\none\ntwo\n\nthree\n", 42);
        check(lines.equals(List.of("one", "two", "three")), "plain output: " + lines);
        // Only informational lines of its own: its last lines, not other output.
        lines = Dexopt.failureLines(
                "stray\n11-23 02:04:58.949 42 43 I dex2oat.exe: x.cc:1 started\n", 42);
        check(lines.size() == 1 && lines.get(0).endsWith("started"), "its own last lines: " + lines);
        check(Dexopt.failureLines("", 42).isEmpty(), "no output");

        System.out.println("DexoptFailureOutputHost: " + checks + " checks passed");
    }
}
