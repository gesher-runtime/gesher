/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class PackageUidIsolationHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        android.app.PropertyInvalidatedCache.setTestMode(true);
        var field = android.content.pm.PackageManager.class.getDeclaredField("sPackageInfoCache");
        field.setAccessible(true);
        ((android.app.PropertyInvalidatedCache<?, ?>)field.get(null)).testPropertyName();
        if (args.length > 2) {
            gesher.services.PackageUidIsolationContractTest.transactionPhase(args[1], args[2]);
        } else {
            gesher.services.PackageUidIsolationContractTest.run(false, args[1]);
        }
        System.out.println("Production identity registry and permission policy; verified signing fixture, real filesystem, constructed manifests and native identity adapter. No Windows/ART or Binder transport claim.");
    }
}
