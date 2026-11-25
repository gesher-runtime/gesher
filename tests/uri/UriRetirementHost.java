/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/** The existing JNI identity boundary; complete Java services execute unchanged. */
public final class UriRetirementHost {
    public static void initializeNativeBoundary(String library) throws Exception {
        System.load(library);
        // Reuse the maintained persistence host's official nonce test boundary.
        // Publication still executes real cache invalidation; only its native
        // shared-memory transport is unavailable in this Linux/JVM fixture.
        android.app.PropertyInvalidatedCache.setTestMode(true);
        var field = android.content.pm.PackageManager.class.getDeclaredField("sPackageInfoCache");
        field.setAccessible(true);
        ((android.app.PropertyInvalidatedCache<?, ?>) field.get(null)).testPropertyName();
    }

    public static void main(String[] args) throws Exception {
        initializeNativeBoundary(args[0]);
        if (args.length > 2 && "--benchmark".equals(args[1])) {
            gesher.services.UriRetirementBenchmark.run(args[2]);
            return;
        }
        if (args.length > 1 && "--storage".equals(args[1])) {
            gesher.services.UriRetirementBenchmark.storage();
            return;
        }
        if (args.length > 2 && "--count-work".equals(args[1])) {
            gesher.services.UriRetirementWorkTest.run("baseline".equals(args[2]));
            return;
        }
        if (args.length > 1 && "--existing-contracts".equals(args[1])) {
            gesher.services.UriGrantContractTest.contracts(null, false);
            gesher.services.IntentUriGrantContractTest.contracts(false);
            gesher.services.PersistableUriGrantContractTest.contracts(false);
            com.gesher.server.UriPermissionModesOracleTest.main(new String[]{args[0]});
        }
        gesher.services.UriGrantRetirementContractTest.contracts(false);
        System.out.println("URI_RETIREMENT_COMPLETE actual_services=true owner_tables_retired=true "
                + "native_boundary=controlled_identity remote_binder=false windows_art=false");
    }
}
