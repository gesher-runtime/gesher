/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class PersistableUriGrantHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        // The official instrumentation hook keeps real invalidation transitions
        // without requiring Android's native shared nonce mapping on the host.
        android.app.PropertyInvalidatedCache.setTestMode(true);
        var field = android.content.pm.PackageManager.class.getDeclaredField("sPackageInfoCache");
        field.setAccessible(true);
        ((android.app.PropertyInvalidatedCache<?, ?>)field.get(null)).testPropertyName();
        gesher.services.PersistableUriGrantContractTest.contracts(false);
        System.out.println("Actual production grant/provider/permission code; host Binder identity, no Binder transport or ART.");
    }
}
