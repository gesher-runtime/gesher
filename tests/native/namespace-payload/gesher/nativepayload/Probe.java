/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.nativepayload;

public final class Probe {
    public static void load(String path) { System.load(path); }
    public static void loadLibrary(String name) { System.loadLibrary(name); }
    public static native int value();
    public static native int constructors();
    public static native int onloads();
    public static native int applicationTargetSdk();
    public static native int helperVmIdentity();
}
