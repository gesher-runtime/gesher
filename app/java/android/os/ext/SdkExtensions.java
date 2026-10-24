/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.os.ext;

import java.util.Collections;
import java.util.Map;

/**
 * A placeholder for the SdkExtensions module's SdkExtensions (framework.jar compiles
 * against the module's API stubs and does not contain it). AndroidX's BuildCompat reads
 * the extension versions in its static initializer (docs/e2e.md). Gesher has none of the
 * updatable modules whose APIs the extensions count, so every extension is at version 0.
 * When a module jar on the boot class path brings the real class, it wins.
 */
public class SdkExtensions {
    /** As SdkExtensions.AD_SERVICES. */
    public static final int AD_SERVICES = 1000000;

    private SdkExtensions() {
    }

    /** The version of {@code extension} (an SDK level from R on): 0, none of its modules. */
    public static int getExtensionVersion(int extension) {
        if (extension < 30) {
            throw new IllegalArgumentException("not a valid extension: " + extension);
        }
        return 0;
    }

    /** Every extension's version: none. */
    public static Map<Integer, Integer> getAllExtensionVersions() {
        return Collections.emptyMap();
    }
}
