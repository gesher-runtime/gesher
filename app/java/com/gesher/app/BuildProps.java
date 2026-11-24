/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

import android.os.SystemProperties;

import java.io.BufferedReader;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;

/**
 * Loads a build.prop ({@code key=value} lines, {@code #} comments) into the system
 * properties, as init does on a device from /system/build.prop. Must run before
 * {@code android.os.Build} is initialized: its fields are read once, in its static
 * initializer.
 */
final class BuildProps {
    private BuildProps() {
    }

    /**
     * ro.opengles.version only as a version Gesher serves, 0x30000 or 0x20000
     * (compat/bionic/egl_gles_version.h; docs/system-features.md): another value would be
     * declared while EGL gives something else, so it is left unset and the system server's
     * probe declares the version. compat/art/jni_invocation_static.cc does the same.
     */
    static boolean servedGlEsVersion(String key, String value) {
        if (!"ro.opengles.version".equals(key)) return true;
        try {
            int v = Integer.decode(value);
            return v == 0x30000 || v == 0x20000;
        } catch (NumberFormatException e) {
            return false;
        }
    }

    static int load(String path) throws IOException {
        int count = 0;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(
                new FileInputStream(path), StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) {
                    continue;
                }
                int eq = line.indexOf('=');
                if (eq <= 0) {
                    continue;
                }
                String key = line.substring(0, eq).trim();
                String value = line.substring(eq + 1).trim();
                // init's "import" and "?=" (set if unset) forms are not supported; a
                // "?=" line is taken as a plain assignment.
                if (key.endsWith("?")) {
                    key = key.substring(0, key.length() - 1).trim();
                }
                if (!servedGlEsVersion(key, value)) {
                    Main.log("build.prop: " + key + "=" + value + " is not an OpenGL ES version"
                            + " Gesher serves (0x30000, 0x20000); left unset, the probe declares it");
                    continue;
                }
                // dalvikvm loads $ANDROID_ROOT/build.prop before the runtime starts
                // (compat/art/jni_invocation_static.cc); ro.* cannot be set twice.
                if (value.equals(SystemProperties.get(key))) {
                    count++;
                    continue;
                }
                try {
                    SystemProperties.set(key, value);
                    count++;
                } catch (RuntimeException e) {
                    Main.log("property " + key + " not set: " + e);
                }
            }
        }
        return count;
    }
}
