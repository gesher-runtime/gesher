/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.io.File;

/**
 * Gesher Camera ({@code gesher-camera.apk} next to dalvikvm.exe, app/gesher-camera; hwsvc2
 * track, ADR 0051), the camera app MediaStore.ACTION_IMAGE_CAPTURE resolves to: the
 * persistent device installs (or updates) it at its start when Camera2 has cameras, and
 * uninstalls it when there are none, as a device without a camera has no camera app.
 */
final class CameraApp {
    static final String PACKAGE = "com.gesher.camera";

    private CameraApp() {
    }

    static void sync(Installer installer, PackageManagerService pm) {
        try {
            String exe = Zygote.exePath();
            String dir = exe.substring(0, exe.lastIndexOf('\\') + 1);
            // Drive-less, with '/', as the launcher hands APK paths over.
            String apk = (dir + "gesher-camera.apk").replace('\\', '/').replaceFirst("^[A-Za-z]:", "");
            int cameras = WindowsCamera.devices().size();
            boolean installed = pm.get(PACKAGE) != null;
            if (cameras > 0 && new File(apk).isFile()) {
                String[] r = installer.install(apk);
                SystemServer.log("camera app: " + PACKAGE + " " + r[1] + " (" + cameras
                        + " camera(s))");
            } else if (cameras == 0 && installed) {
                installer.uninstall(PACKAGE);
                SystemServer.log("camera app: no camera, " + PACKAGE + " uninstalled");
            } else {
                SystemServer.log("camera app: " + (cameras == 0 ? "no camera"
                        : "no gesher-camera.apk") + ", none installed");
            }
        } catch (Throwable t) {
            SystemServer.log("camera app: " + t);
        }
    }
}
