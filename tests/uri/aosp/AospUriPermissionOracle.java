/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.android.server.uri;

import android.net.Uri;

/** Access to the unmodified pinned AOSP state machine without constructing Binder tokens. */
public final class AospUriPermissionOracle {
    private final UriPermission permission = new UriPermission("fixture.source", "fixture.target",
            10200, new GrantUri(0, Uri.parse("content://fixture.uri/item"), 0));
    private final UriPermissionOwner[] owners = {
        new UriPermissionOwner(unused -> { }, "first"),
        new UriPermissionOwner(unused -> { }, "second")
    };

    public void grant(int flags, int owner) {
        permission.grantModes(flags, owner < 0 ? null : owners[owner]);
    }
    public boolean take(int flags) { return permission.takePersistableModes(flags); }
    public boolean release(int flags) { return permission.releasePersistableModes(flags); }
    public boolean revoke(int flags, boolean includingOwners) {
        return permission.revokeModes(flags, includingOwners);
    }
    public void retire(int owner, int modes) { owners[owner].removeUriPermission(permission.uri, modes); }
    public void restore(int modes, long time) { permission.initPersistedModes(modes, time); }
    public long time() { return permission.persistedCreateTime; }
    public int[] modes() {
        return new int[] {permission.modeFlags, permission.ownedModeFlags, permission.globalModeFlags,
                permission.persistableModeFlags, permission.persistedModeFlags};
    }
    public int strength(int modes) { return permission.getStrength(modes); }
}
