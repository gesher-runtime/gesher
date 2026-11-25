/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.Intent;
import android.os.IBinder;

/** Ordinary same-loader host calls; no permission or grant implementation lives here. */
public final class UriRetirementCaller {
    public static IBinder publish(UriGrants grants, int fromUid, String target, Intent intent) {
        IBinder owner = grants.newOwner();
        try {
            grants.publish(owner, grants.prepareIntent(fromUid, target, intent));
            return owner;
        } catch (RuntimeException | Error failure) {
            grants.revokeByOwner(owner);
            throw failure;
        }
    }

    public static void retire(UriGrants grants, IBinder owner) { grants.revokeByOwner(owner); }

    public static void publishPackage(PackageManagerService packages, ApkPackage owner) {
        packages.publishPrepared(owner);
    }
}
