/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class PackageEnabledStateHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        // Exercise real cache invalidations with process-local nonces, as AOSP
        // instrumentation does. Host JVMs do not map Android's shared nonce area.
        android.app.PropertyInvalidatedCache.setTestMode(true);
        var cacheField = android.content.pm.PackageManager.class.getDeclaredField("sPackageInfoCache");
        cacheField.setAccessible(true);
        ((android.app.PropertyInvalidatedCache<?, ?>)cacheField.get(null)).testPropertyName();
        int checks = gesher.services.PackageEnabledStateContractTest.run(false, args[1]);
        System.out.println("Package enabled state host passed: " + checks + " checks; group=" + args[1]);
        System.out.println("Actual service/framework/public-manager code and filesystem; constructed manifests, native identity adapter. No Windows or Binder transport claim.");
    }
}
