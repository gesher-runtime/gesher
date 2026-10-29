/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher;

import com.gesher.app.VmCommandLine;

public final class LauncherCommandLineTest {
    private static final String MAIN = "com.gesher.app.Launcher";
    private static int checks;

    private static void prefix(String expected, String command) {
        String actual = VmCommandLine.beforeMainClass(command, MAIN);
        if (!expected.equals(actual)) throw new AssertionError(command + " -> " + actual);
        ++checks;
    }

    private static void absent(String command) {
        try {
            VmCommandLine.beforeMainClass(command, MAIN);
        } catch (IllegalStateException expected) {
            ++checks;
            return;
        }
        throw new AssertionError("matched a partial or embedded class token: " + command);
    }

    public static void main(String[] args) {
        String vm = "\"C:\\Gesher Runtime\\dalvikvm.exe\" \"-Xbootclasspath:C:/Gesher Runtime/core.jar\""
                + " \"-Ximage:C:/Gesher Runtime/boot.art\" \"-Xint\" \"-Xusejit:false\"";
        prefix(vm, vm + " \"" + MAIN + "\" \"launch\" \"com.gesher.test/.MainActivity\"");
        prefix(vm, vm + " \"" + MAIN + "\"");
        prefix(vm, vm + " " + MAIN + " launch");
        prefix(vm, vm + " " + MAIN);
        prefix(vm, vm + "\t\t\"" + MAIN + "\"\tlaunch");
        prefix(vm, vm + " com.gesher.\"app\".Launcher launch");
        prefix("dalvikvm.exe \"-Dtext=embedded " + MAIN + " token\"",
                "dalvikvm.exe \"-Dtext=embedded " + MAIN + " token\" \"" + MAIN + "\" launch");
        prefix("dalvikvm.exe \"-Dpath=C:\\\\end\\\\\"",
                "dalvikvm.exe \"-Dpath=C:\\\\end\\\\\" \"" + MAIN + "\" launch");
        absent("dalvikvm.exe \"-Dtext=embedded " + MAIN + " token\" OtherMain");
        absent("dalvikvm.exe \"" + MAIN + ".Suffix\" launch");
        absent("dalvikvm.exe prefix" + MAIN + " launch");
        absent("dalvikvm.exe \"\\\"" + MAIN + "\\\"\" launch");
        absent("\"" + MAIN + "\" launch"); // the executable is never the main class
        StackTraceElement[] delegated = {
                new StackTraceElement("com.gesher.server.SystemServer", "main", "SystemServer.java", 1),
                new StackTraceElement("gesher.fixture.Writer", "main", "Writer.java", 1),
                new StackTraceElement("unrelated.Native", "main", "Native.java", 1)};
        String delegatedVm = VmCommandLine.beforeEntrypoint(vm + " gesher.fixture.Writer server", delegated);
        if (!vm.equals(delegatedVm)) throw new AssertionError("delegated main prefix: " + delegatedVm);
        ++checks;
        try {
            VmCommandLine.beforeEntrypoint(vm + " OtherMain", delegated);
            throw new AssertionError("missing actual entrypoint accepted");
        } catch (IllegalStateException expected) { ++checks; }
        System.out.println("LauncherCommandLineTest PASS " + checks
                + " quoted/bare main tokens, tabs, Windows escapes and false-match rejection");
    }
}
