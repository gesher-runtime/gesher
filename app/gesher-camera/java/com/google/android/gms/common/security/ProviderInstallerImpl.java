/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.common.security;

import android.content.Context;

/**
 * Play services' security provider installer, as the client library's ProviderInstaller
 * loads it from Play services' package context (com.gesher.server.GmsPackageSupport: the
 * synthetic Play services package's code is this APK). Play services puts its own copy of
 * Conscrypt first; Gesher's default provider already is Conscrypt (AndroidOpenSSL), so
 * there is nothing to insert. Without the class, apps that insist on a "patched" SSL stack
 * (Gmail's mail server check) refuse to connect.
 */
public final class ProviderInstallerImpl {
    private ProviderInstallerImpl() {
    }

    public static void insertProvider(Context context) {
    }

    public static void reportRequestStats2(Context context, long start, long end) {
    }
}
