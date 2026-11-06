/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
import com.gesher.server.GesherService;

/** Host-shell-only control of the two synthetic fixture packages on a fresh device. */
public final class WindowPromotionControl {
    public static void main(String[] args) throws Exception {
        android.system.Os.setenv("GESHER_BINDER_UID", "2000", true);
        System.loadLibrary("android_runtime");
        if (args.length != 1) throw new IllegalArgumentException("explicit-headless");
        String target = "com.gesher.windowpromotion.target";
        if (args[0].equals("explicit-headless")) {
            GesherService.Client control = GesherService.Client.find();
            if (control == null) throw new IllegalStateException("No fixture device");
            control.setWindowMode(target, "headless");
            System.out.println("promotion: explicit-headless policy set");
        } else throw new IllegalArgumentException("Unknown fixture command");
    }
}
