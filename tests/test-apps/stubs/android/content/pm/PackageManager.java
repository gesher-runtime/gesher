/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.content.pm;

public class PackageManager {
    public static final long GET_ACTIVITIES = 1;
    public static final long GET_RECEIVERS = 2;
    public static final long GET_SERVICES = 4;
    public static final long GET_PROVIDERS = 8;
    public static final long GET_SIGNATURES = 64;
    public static final long GET_SIGNING_CERTIFICATES = 134217728;
    public static final int COMPONENT_ENABLED_STATE_DEFAULT = 0;
    public static final int COMPONENT_ENABLED_STATE_ENABLED = 1;

    public static class NameNotFoundException extends Exception {
        public NameNotFoundException(String name) {
            super(name);
        }
    }

    /** No package is installed in this host fixture. */
    public PackageInfo getPackageInfo(String packageName, int flags) throws NameNotFoundException {
        throw new NameNotFoundException(packageName);
    }
}
