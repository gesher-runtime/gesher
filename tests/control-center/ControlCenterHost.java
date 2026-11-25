/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.io.File;
import java.nio.file.Files;

/** Host boundaries do not replace production file publication or cleanup. */
public final class ControlCenterHost {
    public static void main(String[] args) throws Exception {
        Device.directory = Files.createTempDirectory("gesher-panel-open-").toFile();
        File requests = new File(Device.directory, ControlCenter.REQUESTS);
        try {
            Zygote.exe = null;
            if (ControlCenter.open("com.example.app") || (requests.isDirectory() && requests.list().length != 0)) {
                throw new AssertionError("Unavailable launcher left a request behind");
            }
            Zygote.exe = "C:\\Gesher\\dalvikvm.exe";
            WindowsHost.opened = false;
            if (ControlCenter.open("com.example.app") || (requests.isDirectory() && requests.list().length != 0)) {
                throw new AssertionError("Failed shell launch left a request behind");
            }
            WindowsHost.fail = true;
            if (ControlCenter.open("com.example.app") || (requests.isDirectory() && requests.list().length != 0)) {
                throw new AssertionError("Throwing shell launch left a request behind");
            }
            WindowsHost.fail = false;
            WindowsHost.opened = true;
            if (!ControlCenter.open("com.example.app") || !(requests.isDirectory() && requests.list().length != 0)) {
                throw new AssertionError("Successful shell launch lost its request");
            }
            gesher.services.ControlCenterRequestsTest.main(new String[0]);
            System.out.println("ControlCenterHost all passed: unavailable, failed, throwing and successful launcher");
        } finally {
            ControlCenter.clearResults(Device.directory.getPath());
            requests.delete();
            requests.getParentFile().delete();
            Device.directory.delete();
        }
    }
}
class SystemServer { static void log(String message) { } }
class Device { static File directory; static String dataDir() { return directory.getPath(); } }
class Zygote { static String exe; static String exePath() { return exe; } }
class WindowsHost {
    static boolean opened, fail;
    static String fullPath(String path) { return path; }
    static boolean openUrl(String path) {
        if (fail) throw new IllegalStateException("host launcher unavailable");
        return opened;
    }
}
