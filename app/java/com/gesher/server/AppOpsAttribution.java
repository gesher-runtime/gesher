/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.AttributionSourceState;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Objects;
import java.util.Set;

/** Immutable-by-ownership copies for registered attribution chains. No caller arrays are retained. */
final class AppOpsAttribution {
    private AppOpsAttribution() { }
    static List<AttributionSourceState> chain(AttributionSourceState source) {
        Objects.requireNonNull(source);
        List<AttributionSourceState> result = new ArrayList<>();
        Set<AttributionSourceState> seen = Collections.newSetFromMap(new IdentityHashMap<>());
        for (AttributionSourceState current = source; current != null;) {
            if (!seen.add(current) || current.next != null && current.next.length > 1) {
                throw new IllegalArgumentException("Attribution must be a non-cyclic chain");
            }
            result.add(current);
            current = current.next == null || current.next.length == 0 ? null : current.next[0];
        }
        return result;
    }
    static AttributionSourceState copy(AttributionSourceState source, boolean retainRootToken) {
        List<AttributionSourceState> chain = chain(source);
        AttributionSourceState next = null;
        for (int index = chain.size() - 1; index >= 0; index--) {
            AttributionSourceState original = chain.get(index);
            AttributionSourceState saved = new AttributionSourceState();
            saved.uid = original.uid; saved.pid = original.pid; saved.deviceId = original.deviceId;
            saved.packageName = original.packageName; saved.attributionTag = original.attributionTag;
            saved.token = index == 0 && !retainRootToken ? null : original.token;
            saved.renouncedPermissions = original.renouncedPermissions == null ? null : original.renouncedPermissions.clone();
            saved.next = next == null ? new AttributionSourceState[0] : new AttributionSourceState[]{next};
            next = saved;
        }
        return next;
    }
    static boolean equalExceptRootToken(AttributionSourceState first, AttributionSourceState second) {
        List<AttributionSourceState> a = chain(first), b = chain(second);
        if (a.size() != b.size()) return false;
        for (int i = 0; i < a.size(); i++) {
            AttributionSourceState x = a.get(i), y = b.get(i);
            if (x.uid != y.uid || x.pid != y.pid || x.deviceId != y.deviceId
                    || !Objects.equals(x.packageName, y.packageName) || !Objects.equals(x.attributionTag, y.attributionTag)
                    || !Arrays.equals(x.renouncedPermissions, y.renouncedPermissions)
                    || i != 0 && !Objects.equals(x.token, y.token)) return false;
        }
        return true;
    }
}
