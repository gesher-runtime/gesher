/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.ClipData;
import android.content.ContentProvider;
import android.content.Intent;
import android.net.Uri;
import android.os.BadParcelableException;
import android.os.IBinder;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.IdentityHashMap;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.WeakHashMap;

/** Historical caller capabilities for one managed Activity, not live URI grants. */
final class ActivityCallerState {
    private static final int READ = Intent.FLAG_GRANT_READ_URI_PERMISSION;
    private static final int WRITE = Intent.FLAG_GRANT_WRITE_URI_PERMISSION;

    /** Supplied only after authenticating the process or a server-held creator capability. */
    static final class Origin {
        final ApkPackage code;
        final int uid;
        final String packageName;
        final boolean system;
        final boolean shareIdentity;

        Origin(ApkPackage code, int uid, String packageName, boolean system,
                boolean shareIdentity) {
            this.code = code;
            this.uid = uid;
            this.packageName = packageName;
            this.system = system;
            this.shareIdentity = shareIdentity;
        }
    }

    private record ContentUri(int userId, Uri uri, boolean prefix) {
        static ContentUri from(Uri uri, int defaultUserId, int flags) {
            return new ContentUri(ContentProvider.getUserIdFromUri(uri, defaultUserId),
                    ContentProvider.getUriWithoutUserId(uri),
                    (flags & Intent.FLAG_GRANT_PREFIX_URI_PERMISSION) != 0);
        }
    }

    static final class Snapshot {
        final int uid;
        final String packageName;
        final boolean shareIdentity;
        private final Map<ContentUri, Integer> permissions;

        private Snapshot(Origin origin, Map<ContentUri, Integer> permissions) {
            uid = origin.uid;
            packageName = origin.packageName;
            shareIdentity = origin.shareIdentity;
            this.permissions = Collections.unmodifiableMap(new HashMap<>(permissions));
        }

        boolean visibleTo(String queryingPackage, int queryingUid, boolean internal) {
            // In legacy mode unrelated packages all have UID 10000. That runtime
            // implementation detail is not shared Android application ownership.
            // Real shared-UID packages and platform signing are not implemented.
            return internal || shareIdentity || packageName != null
                    && packageName.equals(queryingPackage) && uid == queryingUid;
        }

        boolean allows(Uri uri, int flags, int userId) {
            Integer access = permissions.get(ContentUri.from(uri, userId, flags));
            if (access == null) {
                throw new IllegalArgumentException("The content URI was not delivered in this caller's Intent");
            }
            int requested = flags & (READ | WRITE);
            return (access & requested) == requested;
        }
    }

    private final Map<IBinder, Snapshot> callers = new HashMap<>();

    synchronized void add(IBinder token, Snapshot snapshot) {
        callers.put(Objects.requireNonNull(token), Objects.requireNonNull(snapshot));
    }

    synchronized Snapshot get(IBinder token) {
        return token == null ? null : callers.get(token);
    }

    /** The caller must hold the package monitor while capturing installed permission state. */
    static Snapshot capture(Origin origin, Intent intent, UriGrants grants) {
        Map<ContentUri, Integer> permissions = new HashMap<>();
        for (Uri uri : contentUris(intent)) {
            ContentUri key = ContentUri.from(uri, 0, 0);
            int access = 0;
            // System/server origins have the real control-plane authority used by
            // URI grants. App authority always comes from its installed generation.
            if (origin.system || grants.canAccess(origin.code, key.uri, READ, key.userId)) access |= READ;
            if (origin.system || grants.canAccess(origin.code, key.uri, WRITE, key.userId)) access |= WRITE;
            permissions.put(key, access); // Keep delivered-but-denied distinct from absent.
        }
        return new Snapshot(origin, permissions);
    }

    static void validateQuery(Uri uri, int flags, int userId) {
        if (uri == null || !"content".equals(uri.getScheme()) || !Intent.isAccessUriMode(flags)) {
            throw new IllegalArgumentException("A content URI and read and/or write access are required");
        }
        // The runtime has one Android user. Never reinterpret an unsupported user
        // or an ambiguous path as an authorized URI in the current user.
        UriGrants.checkedUri(uri, userId);
    }

    private static Set<Uri> contentUris(Intent root) {
        Set<Uri> result = new HashSet<>();
        if (root == null) return result;
        ArrayDeque<Intent> pending = new ArrayDeque<>();
        Set<Intent> visited = Collections.newSetFromMap(new IdentityHashMap<>());
        pending.add(root);
        while (!pending.isEmpty()) {
            Intent intent = pending.removeFirst();
            if (!visited.add(intent)) continue;
            add(result, intent.getData());
            if (intent.hasExtra(Intent.EXTRA_STREAM)) {
                ArrayList<Uri> streams = null;
                try { streams = intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM, Uri.class); }
                catch (BadParcelableException malformed) {
                    SystemServer.log("ComponentCaller: malformed EXTRA_STREAM list");
                }
                if (streams != null) {
                    for (Uri uri : streams) add(result, uri);
                } else {
                    try { add(result, intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri.class)); }
                    catch (BadParcelableException malformed) {
                        SystemServer.log("ComponentCaller: malformed EXTRA_STREAM URI");
                    }
                }
            }
            ClipData clip = intent.getClipData();
            if (clip == null) continue;
            for (int i = 0; i < clip.getItemCount(); i++) {
                ClipData.Item item = clip.getItemAt(i);
                add(result, item.getUri());
                if (item.getIntent() != null) pending.addLast(item.getIntent());
            }
        }
        return result;
    }

    private static void add(Set<Uri> uris, Uri uri) {
        if (uri != null && "content".equals(uri.getScheme())) uris.add(uri);
    }
}
