/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;
/** Uses only the existing production toastTag API, so unchanged source is a real control. */
public final class ToastTagProbe {
    public static void main(String[] args) {
        String first = "Aa".repeat(40), second = "BB".repeat(40);
        if (first.hashCode() != second.hashCode()) throw new AssertionError("Invalid collision fixture");
        String a = NotificationManagerService.toastTag(first, 7), b = NotificationManagerService.toastTag(second, 7);
        if (a.equals(b)) throw new AssertionError("Distinct long Android tags alias the same Windows toast identity");
        if (a.length() > 64 || b.length() > 64) throw new AssertionError("Windows tag limit exceeded");
        System.out.println("ToastTagProbe passed");
    }
}
