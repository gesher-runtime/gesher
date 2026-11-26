/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.io.File;
import java.util.Objects;
import java.util.function.Predicate;

/** Internal ownership lookup shared by providers that accept existing shared-storage files. */
final class SharedStorageOwnership {
    interface Lookup {
        /**
         * Returns the recorded owner, or null for an unindexed or ownerless file. The lookup
         * includes pending and trashed rows and must not apply caller visibility filters.
         * An unavailable or failed lookup must throw instead of returning null.
         */
        String ownerOf(File canonicalFile);
    }

    private static volatile Lookup sLookup;

    private SharedStorageOwnership() { }

    /** Installed by the media provider before it is published. */
    static void register(Lookup lookup) {
        sLookup = Objects.requireNonNull(lookup);
    }

    /**
     * The predicate recognizes every installed package belonging to the caller's UID,
     * including shared-UID siblings. A runtime without a media provider has no records.
     */
    static void enforceOwner(File canonicalFile, Predicate<String> uidOwnsPackage) {
        Lookup lookup = sLookup;
        if (lookup == null) return;
        final String owner;
        final boolean authorized;
        try {
            owner = lookup.ownerOf(canonicalFile);
            authorized = owner == null || uidOwnsPackage.test(owner);
        } catch (RuntimeException failure) {
            throw new SecurityException("Unable to verify MediaStore file ownership", failure);
        }
        if (!authorized) throw new SecurityException("Caller does not own this MediaStore file");
    }
}
