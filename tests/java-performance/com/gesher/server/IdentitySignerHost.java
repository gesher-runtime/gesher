/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

/** Host-only caller in the actual service loader/package; no reflective timing adapter. */
public final class IdentitySignerHost {
    private IdentitySignerHost() { }
    public static String signer(ApkPackage pkg) { return AppOpsModeStore.signer(pkg); }
}
