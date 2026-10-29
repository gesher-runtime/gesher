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
        String ambiguous = "dalvikvm.exe -cp gesher.fixture.Writer -Dk=gesher.fixture.Writer "
                + "gesher.fixture.Writer gesher.fixture.Writer";
        String indexed = VmCommandLine.beforeArgument(ambiguous, 4);
        if (!indexed.equals("dalvikvm.exe -cp gesher.fixture.Writer -Dk=gesher.fixture.Writer")) {
            throw new AssertionError("incorrect parser-selected prefix: " + indexed);
        }
        ++checks;
        try {
            VmCommandLine.beforeArgument(ambiguous, 99);
            throw new AssertionError("out-of-range entrypoint accepted");
        } catch (IllegalStateException expected) { ++checks; }
        System.out.println("LauncherCommandLineTest PASS " + checks
                + " quoted/bare main tokens, tabs, Windows escapes and false-match rejection");
    }
}
