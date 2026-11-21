/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.system.Os;
import android.system.StructUtsname;

/**
 * The operating system an app sees through Java: Android's kernel is Linux, so libcore's
 * os.name (from uname's sysname) is "Linux" and Os.uname() agrees with it. Libraries
 * select native code from it (JNA loads android-x86-64 rather than win32-x86-64). The
 * release names no real kernel, so kernel-version checks keep failing.
 */
public final class OsIdentityContractTest {
    private static int checks;

    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }

    public static void main(String[] args) throws Exception {
        StructUtsname uts = Os.uname();
        require("Linux".equals(uts.sysname), "uname sysname is " + uts.sysname + ", not Linux");
        require("Linux".equals(System.getProperty("os.name")),
                "os.name is " + System.getProperty("os.name") + ", not Linux");
        require(uts.release.equals(System.getProperty("os.version")),
                "os.version differs from uname release");
        require(uts.release.startsWith("0."), "uname release claims a kernel version: " + uts.release);
        require("x86_64".equals(uts.machine) && "x86_64".equals(System.getProperty("os.arch")),
                "machine/os.arch is not x86_64");
        require("Dalvik".equals(System.getProperty("java.vm.name")), "java.vm.name is not Dalvik");
        System.out.println("OsIdentityContractTest: os.name=Linux, os.version=" + uts.release);
        System.out.println("OsIdentityContractTest all passed: " + checks + " checks");
    }
}
