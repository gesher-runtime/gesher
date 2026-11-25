/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.util.Properties;

/**
 * The Control Center's camera setting "Allow choosing an image from the PC instead of taking
 * a photo" (docs/devices.md): {@code image_picker_fallback=true} in
 * {@code <data>/system/camera.properties}, off when absent. The system server reads it
 * ({@link CameraSourceService} answers app processes, which have no access to data/system);
 * each read sees the file's current state, so a change applies to the next camera use.
 */
public final class CameraImageFallback {
    static final String KEY = "image_picker_fallback";

    /** Host tests set the data directory instead of {@link Device#dataDir()}. */
    static volatile String sDataDirForTest;

    private CameraImageFallback() {
    }

    static File file() {
        String dataDir = sDataDirForTest != null ? sDataDirForTest : Device.dataDir();
        return dataDir == null || dataDir.isEmpty() ? null : new File(dataDir, "system/camera.properties");
    }

    /** Whether the setting is on; false when the file is missing or unreadable. */
    public static boolean enabled() {
        try {
            File file = file();
            if (file == null || !file.isFile()) return false;
            Properties props = new Properties();
            try (FileInputStream in = new FileInputStream(file)) {
                props.load(in);
            }
            return "true".equalsIgnoreCase(props.getProperty(KEY, "").trim());
        } catch (IOException | RuntimeException | LinkageError e) {
            return false;
        }
    }
}
