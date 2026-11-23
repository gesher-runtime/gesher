/*
 * Copyright (C) 2018 The Android Open Source Project
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.Intent;
import java.util.HashSet;
import java.util.Set;

/**
 * Mode/owner transitions from AOSP UriPermission at frameworks/base
 * 45034f0663f960d9ee5fb0a101a4732b71f6e2f4. The service owns synchronization;
 * owner membership uses the same lock as the canonical grant table, so owner
 * retirement never needs a second lock or a callback into a locked owner.
 */
final class UriPermissionModes<T> {
    static final int READ = Intent.FLAG_GRANT_READ_URI_PERMISSION;
    static final int WRITE = Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
    static final int ACCESS = READ | WRITE;
    static final long INVALID_TIME = Long.MIN_VALUE;
    static final int NONE = 0, OWNED = 1, GLOBAL = 2, PERSISTABLE = 3;

    int global;
    int persistable;
    int persisted;
    long persistedTime = INVALID_TIME;
    private Set<T> readOwners;
    private Set<T> writeOwners;

    int owned() {
        return (readOwners == null || readOwners.isEmpty() ? 0 : READ)
                | (writeOwners == null || writeOwners.isEmpty() ? 0 : WRITE);
    }

    /** An offer to persist is not an active owner/global/persisted mode. */
    int active() { return owned() | global | persisted; }

    int strength(int flags) {
        int access = flags & ACCESS;
        if ((persistable & access) == access) return PERSISTABLE;
        if ((global & access) == access) return GLOBAL;
        if ((owned() & access) == access) return OWNED;
        return NONE;
    }

    void grant(int flags, T owner) {
        int access = flags & ACCESS;
        if ((flags & Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION) != 0) persistable |= access;
        if (owner == null) {
            global |= access;
        } else {
            if ((access & READ) != 0) {
                if (readOwners == null) readOwners = new HashSet<>();
                readOwners.add(owner);
            }
            if ((access & WRITE) != 0) {
                if (writeOwners == null) writeOwners = new HashSet<>();
                writeOwners.add(owner);
            }
        }
    }

    boolean take(int flags, long now) {
        int access = flags & ACCESS;
        if ((persistable & access) != access) return false;
        int before = persisted;
        persisted |= access;
        if (persisted != 0) persistedTime = now;
        return before != persisted;
    }

    boolean release(int flags) {
        int before = persisted;
        persisted &= ~(flags & ACCESS);
        if (persisted == 0) persistedTime = INVALID_TIME;
        return before != persisted;
    }

    void restore(int flags, long createdTime) {
        persistable = persisted = flags & ACCESS;
        persistedTime = createdTime;
    }

    boolean revoke(int flags, boolean includingOwners) {
        int access = flags & ACCESS;
        int before = persisted;
        if ((flags & Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION) != 0) {
            persistable &= ~access;
            persisted &= ~access;
        }
        global &= ~access;
        if (includingOwners) {
            if ((access & READ) != 0) readOwners = null;
            if ((access & WRITE) != 0) writeOwners = null;
        }
        if (persisted == 0) persistedTime = INVALID_TIME;
        return before != persisted;
    }

    void removeOwner(T owner, int flags) {
        if ((flags & READ) != 0 && readOwners != null) {
            readOwners.remove(owner);
            if (readOwners.isEmpty()) readOwners = null;
        }
        if ((flags & WRITE) != 0 && writeOwners != null) {
            writeOwners.remove(owner);
            if (writeOwners.isEmpty()) writeOwners = null;
        }
    }

    boolean ownedBy(T owner) {
        return (readOwners != null && readOwners.contains(owner))
                || (writeOwners != null && writeOwners.contains(owner));
    }

    Set<T> owners() {
        Set<T> result = new HashSet<>();
        if (readOwners != null) result.addAll(readOwners);
        if (writeOwners != null) result.addAll(writeOwners);
        return result;
    }
}
