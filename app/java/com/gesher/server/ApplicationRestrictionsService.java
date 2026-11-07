/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.IRestrictionsManager;
import android.content.Intent;
import android.os.Bundle;
import android.os.PersistableBundle;

import java.util.Collections;
import java.util.List;

/** This unmanaged desktop has no Android application policy or restrictions provider. */
final class ApplicationRestrictionsService extends IRestrictionsManager.Default {
    @Override public Bundle getApplicationRestrictions(String packageName) {
        return new Bundle();
    }

    @Override public List<Bundle> getApplicationRestrictionsPerAdminForUser(
            int userId, String packageName) {
        return Collections.emptyList();
    }

    @Override public boolean hasRestrictionsProvider() { return false; }

    @Override public Intent createLocalApprovalIntent() { return null; }

    private static UnsupportedOperationException unavailable() {
        return new UnsupportedOperationException(
                "This desktop has no Android restrictions provider or local approval UI");
    }

    @Override public void requestPermission(String packageName, String requestType,
            String requestId, PersistableBundle requestData) {
        throw unavailable();
    }

    @Override public void notifyPermissionResponse(String packageName, PersistableBundle response) {
        throw unavailable();
    }
}
