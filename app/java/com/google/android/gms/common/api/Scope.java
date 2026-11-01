/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.common.api;

import java.util.Objects;

/**
 * An OAuth2 scope requested by an application.
 */
public final class Scope {
    private final String mScopeUri;

    public Scope(String scopeUri) {
        if (scopeUri == null || scopeUri.isEmpty()) {
            throw new IllegalArgumentException("scopeUri must not be empty");
        }
        mScopeUri = scopeUri;
    }

    public String getScopeUri() {
        return mScopeUri;
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) return true;
        if (!(o instanceof Scope)) return false;
        Scope scope = (Scope) o;
        return Objects.equals(mScopeUri, scope.mScopeUri);
    }

    @Override
    public int hashCode() {
        return mScopeUri.hashCode();
    }

    @Override
    public String toString() {
        return mScopeUri;
    }
}
