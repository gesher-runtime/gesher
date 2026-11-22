/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

/** Entry point for the Windows gate; the corpus and production classes share its boot loader. */
public final class NotificationStoreFormatRunner {
    public static void main(String[] args) throws Exception {
        if (args.length != 1 || !(args[0].equals("write") || args[0].equals("read"))) {
            throw new IllegalArgumentException("Expected write or read");
        }
        System.loadLibrary("android_runtime");
        NotificationStoreFormatDevice.contracts(args[0]);
    }
}
