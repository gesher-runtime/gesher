/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.content.pm;

public class ApplicationInfo {
    public static final int FLAG_SYSTEM = 1 << 0;
    public static final int FLAG_INSTALLED = 1 << 29;

    public String packageName;
    public String name;
    public int flags;
    public boolean enabled;
    public int uid;
    public int targetSdkVersion;
    public String sourceDir;
    public String publicSourceDir;
    public String dataDir;
}
