/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

import java.io.File;
import java.util.Locale;

/** ZIP containers holding the APKs for a single app installation. */
public final class PackageBundles {
    private PackageBundles() {}

    public static boolean isBundle(File file) {
        String name = file.getName().toLowerCase(Locale.ROOT);
        return name.endsWith(".apks") || name.endsWith(".xapk")
                || name.endsWith(".apkm") || name.endsWith(".apk+");
    }
}
