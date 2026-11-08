/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import com.gesher.server.Zygote;
import java.io.File;
import java.nio.file.Paths;

/** Java-to-Win32 path identity; actual ShellLink creation remains in DynamicShortcutTest. */
public final class ZygotePathTest {
    private static int checks;

    private static void equal(String expected, String actual, String label) {
        if (!expected.equals(actual)) {
            throw new AssertionError(label + ": expected " + expected + ", got " + actual);
        }
        checks++;
    }

    private static void rejects(Runnable operation, String label) {
        try { operation.run(); }
        catch (IllegalArgumentException expected) { checks++; return; }
        throw new AssertionError(label);
    }

    public static void contracts() {
        String exe = Zygote.exePath();
        if (exe == null || exe.length() < 3 || exe.charAt(1) != ':') {
            throw new AssertionError("Expected a Windows runtime executable path, got " + exe);
        }
        String drive = exe.substring(0, 2);
        equal("C:\\Users\\runneradmin\\AppData\\Local\\Temp\\gms-fixture\\קיצור בדיקה.lnk",
                Zygote.windowsPath("/mnt/c/Users/runneradmin/AppData/Local/Temp/gms-fixture/קיצור בדיקה.lnk"),
                "Other-drive Unicode ShellLink destination");
        equal("C:\\space directory\\quote\"name\\😀.lnk",
                Zygote.windowsPath("/mnt/C/space directory/quote\"name/😀.lnk"),
                "Uppercase drive and exact UTF-16 contents");
        equal("C:\\", Zygote.windowsPath("/mnt/c"), "Mounted drive root");
        equal("C:\\", Zygote.windowsPath("/mnt/C/"), "Mounted drive root with separator");
        equal("Z:\\logs\\output.txt", Zygote.windowsPath("/mnt/z/logs/output.txt"),
                "Mounted drive independent of runtime drive");
        equal("A:\\", Zygote.windowsPath("/mnt/a"), "First ASCII drive");
        equal("Z:\\", Zygote.windowsPath("/mnt/Z"), "Last ASCII drive");
        equal(drive + "\\data\\logs\\output.txt", Zygote.windowsPath("/data/logs/output.txt"),
                "Ordinary rooted path");
        equal(drive + "\\", Zygote.windowsPath("/"), "Ordinary root");
        for (String path : new String[]{"/mnt", "/mnt/sdcard/item", "/mnt/cc/item",
                "/mnt/1/item", "/mnt/é/item", "/mnt/c:other/item", "/mnt/c-other/item",
                "/mnt/c\\folder/file", "/mnt\\c/folder/file"}) {
            equal(drive + path.replace('/', '\\'), Zygote.windowsPath(path),
                    "Ordinary mount-like path " + path);
        }
        String other = Character.toUpperCase(exe.charAt(0)) == 'C' ? "D:" : "C:";
        String suffix = "\\Users\\test user\\קיצור 😀.lnk";
        String guest = Zygote.fromWindowsPath(other + suffix);
        equal("/mnt/" + Character.toLowerCase(other.charAt(0)) + suffix.replace('\\', '/'),
                guest, "Other-drive Java path");
        equal(other + suffix, Zygote.windowsPath(guest), "Other-drive path round trip");
        equal(suffix.replace('\\', '/'), Zygote.fromWindowsPath(drive + suffix),
                "Runtime-drive Java path");
        equal(drive + suffix, Zygote.windowsPath(Zygote.fromWindowsPath(drive + suffix)),
                "Runtime-drive path round trip");
        String relative = new File("relative path/קיצור.lnk").getAbsolutePath();
        equal(Zygote.windowsPath(relative), Zygote.windowsPath("relative path/קיצור.lnk"),
                "Relative path resolves before Windows conversion");
        String unc = "\\\\Server Name\\Share Name\\תיקייה 😀\\file.txt";
        String uncGuest = "/mnt/unc/Server Name/Share Name/תיקייה 😀/file.txt";
        equal(uncGuest, Zygote.fromWindowsPath(unc), "UNC guest namespace");
        equal(uncGuest, new File(uncGuest).getAbsolutePath(), "Java File preserves UNC identity");
        equal(uncGuest, Paths.get(uncGuest).normalize().toString(), "Java Path preserves UNC identity");
        equal(unc, Zygote.windowsPath(uncGuest), "UNC native path round trip");
        equal(unc, Zygote.windowsPath(unc), "Raw native UNC recognized before Java normalization");
        equal(unc, Zygote.windowsPath(unc.replace('\\', '/')), "Raw slash UNC recognized before Java normalization");
        equal(uncGuest, Zygote.fromWindowsPath("\\\\?\\UNC\\" + unc.substring(2)), "Extended UNC path");
        equal(uncGuest, Zygote.fromWindowsPath("\\\\?\\unc\\" + unc.substring(2)), "Extended UNC prefix case");
        equal("\\\\localhost\\C$", Zygote.windowsPath("/mnt/unc/localhost/C$/"), "UNC share root");
        equal(Zygote.fromWindowsPath(other + suffix), Zygote.fromWindowsPath("\\\\?\\" + other + suffix),
                "Extended drive remains a drive path");
        equal(drive + "\\mnt\\uncx\\file", Zygote.windowsPath("/mnt/uncx/file"), "UNC prefix boundary");
        for (String invalid : new String[]{"/mnt/unc", "/mnt/unc/server", "/mnt/unc/./share",
                "/mnt/unc/server/..", "/mnt/unc/?/UNC/share", "/mnt/unc/server/share:stream",
                "/mnt/unc\\server\\share", "/mnt/unc:stream"}) {
            rejects(() -> Zygote.windowsPath(invalid), "Malformed UNC namespace accepted: " + invalid);
        }
        for (String invalid : new String[]{"\\\\server", "\\\\server\\", "\\\\.\\pipe\\name",
                "\\\\?\\GLOBALROOT\\Device\\Volume\\file", "\\\\?\\Volume{123}\\file",
                "\\\\?\\UNC\\server", "\\\\?\\C:relative"}) {
            rejects(() -> Zygote.fromWindowsPath(invalid), "Unsupported native namespace accepted: " + invalid);
        }
        System.out.println("ZygotePathTest all passed: " + checks + " checks");
    }

    public static void main(String[] args) {
        System.loadLibrary("android_runtime");
        contracts();
    }
}
