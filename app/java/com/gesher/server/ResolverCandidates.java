/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.IntentFilter;

/**
 * Android 16 IntentResolver's candidate selection, for queries it answers from its
 * indexes rather than from one package's component list. IntentFilter.match alone
 * skips the action test for an intent without an action and accepts a filter
 * without data for an intent without data; the indexes never offer such pairs.
 *
 * <p>addFilter indexes a filter by scheme, by MIME type and, without either, by
 * action (types also index typed actions). queryIntent then collects scheme
 * candidates, type candidates (by typed action for a "*" base type) and, for an
 * intent with neither type nor scheme, action candidates. IntentFilter.match still
 * decides among the candidates.
 */
final class ResolverCandidates {
    private ResolverCandidates() { }

    static boolean isCandidate(IntentFilter filter, String action, String resolvedType, String scheme) {
        if (scheme != null && filter.hasDataScheme(scheme)) return true;
        int types = filter.countDataTypes();
        if (resolvedType != null && types != 0) {
            int slash = resolvedType.indexOf('/');
            if (slash > 0) {
                // A concrete base type reaches every typed filter IntentFilter.match can accept.
                if (!"*".equals(resolvedType.substring(0, slash))) return true;
                if (action != null && filter.hasAction(action)) return true;
            }
        }
        return resolvedType == null && scheme == null && action != null
                && types == 0 && filter.countDataSchemes() == 0 && filter.hasAction(action);
    }
}
