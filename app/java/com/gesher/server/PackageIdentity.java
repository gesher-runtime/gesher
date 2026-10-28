/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.os.Process;

import java.util.Collection;
import java.util.UUID;

/** Server-owned single-user install identity. Never reconstructed from a Binder hello. */
final class PackageIdentity {
    final String packageName;
    final int uid;
    final String installInstance;

    PackageIdentity(String packageName, int uid, String installInstance) {
        if (packageName == null || packageName.isEmpty()
                || packageName.indexOf('\0') >= 0 || packageName.indexOf('\t') >= 0
                || packageName.indexOf('\n') >= 0
                || packageName.indexOf('\r') >= 0
                || uid < Process.FIRST_APPLICATION_UID || uid > Process.LAST_APPLICATION_UID
                || !UUID.fromString(installInstance).toString().equals(installInstance)) {
            throw new IllegalArgumentException("invalid package install identity");
        }
        this.packageName = packageName;
        this.uid = uid;
        this.installInstance = installInstance;
    }

    static PackageIdentity allocate(String packageName, Collection<PackageIdentity> installed) {
        // AOSP android-16.0.0_r4 AppIdSettingMap: normal application appIds occupy
        // FIRST_APPLICATION_UID..LAST_APPLICATION_UID. Freed appIds may be reused;
        // installInstance distinguishes uninstall/reinstall even when the uid repeats.
        boolean[] used = new boolean[Process.LAST_APPLICATION_UID
                - Process.FIRST_APPLICATION_UID + 1];
        for (PackageIdentity identity : installed) {
            used[identity.uid - Process.FIRST_APPLICATION_UID] = true;
        }
        for (int i = 0; i < used.length; i++) {
            if (!used[i]) return new PackageIdentity(packageName,
                    Process.FIRST_APPLICATION_UID + i, UUID.randomUUID().toString());
        }
        throw new IllegalStateException("no application uid available");
    }
}
