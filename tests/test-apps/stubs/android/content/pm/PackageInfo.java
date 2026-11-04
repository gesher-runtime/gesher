/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.content.pm;

public class PackageInfo {
    public String packageName;
    public int versionCode;
    public String versionName;
    public long firstInstallTime;
    public long lastUpdateTime;
    public ApplicationInfo applicationInfo;
    public Signature[] signatures;
    public SigningInfo signingInfo;
    public ServiceInfo[] services;
    public ActivityInfo[] receivers;

    public void setLongVersionCode(long code) {
        versionCode = (int) code;
    }
}
