/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.util.AtomicFile;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.List;

/**
 * Host-side CLI helper used by {@code gesher-app cameras}. Separated from
 * {@link WindowsCamera} so {@code WindowsCamera} stays free of framework dependencies
 * like {@link AtomicFile}.
 */
public final class CameraSelection {
    private CameraSelection() {
    }

    /** Entry point for {@code gesher-app cameras}. Does not start the device. */
    public static void main(String[] args) throws IOException {
        if (args.length < 1) throw new IllegalArgumentException("Missing camera settings path");
        AtomicFile setting = new AtomicFile(new File(args[0]));
        if (args.length == 2 && args[1].equals("reset")) {
            setting.delete();
            if (setting.exists()) throw new IOException("Could not clear the rear camera setting");
            return;
        }
        System.loadLibrary("android_runtime");
        List<WindowsCamera.Device> devices = WindowsCamera.devices();
        if (args.length == 1) {
            for (int i = 0; i < devices.size(); i++) {
                WindowsCamera.Device d = devices.get(i);
                if (!d.test) {
                    System.out.println(i + "  " + d.name + "  " + d.sizes
                            + (d.isRearFacing() ? "  [rear]" : ""));
                }
            }
            return;
        }
        if (args.length != 3 || !args[1].equals("rear")) {
            throw new IllegalArgumentException("cameras [rear CAMERA_ID | reset]");
        }
        int index = Integer.parseInt(args[2]);
        if (index < 0 || index >= devices.size() || devices.get(index).test) {
            throw new IllegalArgumentException("No physical camera with ID " + index);
        }
        String selection = WindowsCamera.selectionKey(devices.get(index).id);
        FileOutputStream out = setting.startWrite();
        try {
            out.write(selection.getBytes(StandardCharsets.US_ASCII));
        } catch (IOException e) {
            setting.failWrite(out);
            throw e;
        }
        setting.finishWrite(out);
        if (!selection.equals(new String(setting.readFully(), StandardCharsets.US_ASCII))) {
            throw new IOException("Could not save the rear camera setting");
        }
        System.out.println(devices.get(index).name);
    }
}
