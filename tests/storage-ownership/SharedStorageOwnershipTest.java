/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.io.File;
import java.util.Set;
import java.util.concurrent.atomic.AtomicInteger;

/** Synthetic authorization-predicate cases; no app requests or file operations. */
public final class SharedStorageOwnershipTest {
    private static int checks;

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }

    private static SecurityException denied(Runnable action) {
        try { action.run(); }
        catch (SecurityException expected) { checks++; return expected; }
        throw new AssertionError("Ownership check accepted an unauthorized or unverifiable owner");
    }

    public static void main(String[] args) {
        File path = new File("synthetic-owned-file").getAbsoluteFile();
        AtomicInteger lookups = new AtomicInteger();
        SharedStorageOwnership.enforceOwner(path, owner -> {
            throw new AssertionError("No media provider has been installed");
        });
        check(true, "A runtime without a media index has no ownership records");

        IllegalStateException initializing = new IllegalStateException("index not initialized");
        SharedStorageOwnership.register(file -> { throw initializing; });
        check(denied(() -> SharedStorageOwnership.enforceOwner(path, owner -> true)).getCause()
                        == initializing,
                "An installed index that has not initialized fails closed");

        SharedStorageOwnership.register(file -> {
            check(file.equals(path), "The canonical path reaches the lookup unchanged");
            lookups.incrementAndGet();
            return null;
        });
        SharedStorageOwnership.enforceOwner(path, owner -> {
            throw new AssertionError("An ownerless file needs no package comparison");
        });
        check(lookups.get() == 1, "The current media index is consulted once");

        SharedStorageOwnership.register(file -> "owner.package");
        SharedStorageOwnership.enforceOwner(path, "owner.package"::equals);
        check(true, "The owner's UID is authorized");
        denied(() -> SharedStorageOwnership.enforceOwner(path, "unrelated.package"::equals));

        Set<String> sharedUid = Set.of("calling.package", "shared.owner");
        SharedStorageOwnership.register(file -> "shared.owner");
        SharedStorageOwnership.enforceOwner(path, sharedUid::contains);
        check(true, "A sibling package sharing the authenticated UID is authorized");
        denied(() -> SharedStorageOwnership.enforceOwner(path, owner -> false));

        IllegalStateException unavailable = new IllegalStateException("index unavailable");
        SharedStorageOwnership.register(file -> { throw unavailable; });
        check(denied(() -> SharedStorageOwnership.enforceOwner(path, owner -> true)).getCause()
                        == unavailable,
                "A lookup failure is denied and its cause is preserved");
        SharedStorageOwnership.register(file -> "owner.package");
        denied(() -> SharedStorageOwnership.enforceOwner(path, owner -> { throw unavailable; }));
        System.out.println("Shared storage ownership: " + checks + " checks passed");
    }
}
