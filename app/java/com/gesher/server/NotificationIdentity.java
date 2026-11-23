/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

/** File-safe notification identity, retaining every Java string code unit. */
final class NotificationIdentity {
    private NotificationIdentity() { }

    static String digest(String key) {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            // UTF-8 encoders replace unpaired surrogates. Tags are Java strings,
            // so hash their code units directly to keep those identities distinct.
            for (int i = 0; i < key.length(); ++i) {
                char c = key.charAt(i);
                digest.update((byte) (c >>> 8)); digest.update((byte) c);
            }
            StringBuilder hex = new StringBuilder(64);
            for (byte b : digest.digest()) {
                hex.append(Character.forDigit((b >>> 4) & 15, 16));
                hex.append(Character.forDigit(b & 15, 16));
            }
            return hex.toString();
        } catch (NoSuchAlgorithmException impossible) { throw new AssertionError(impossible); }
    }
}
