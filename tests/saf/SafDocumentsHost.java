/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class SafDocumentsHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        // The native host sets the process to the first application UID. Reserve
        // that fixture identity before production allocates any built-in package.
        int hostUid = com.gesher.server.PackageUids.uidFor(
                gesher.services.SafDocumentsContractTest.HOST_PROCESS_PACKAGE);
        if (hostUid != android.os.Process.myUid()) {
            throw new IllegalStateException("SAF host process UID does not match its admitted fixture package");
        }
        android.app.PropertyInvalidatedCache.setTestMode(true);
        var field=android.content.pm.PackageManager.class.getDeclaredField("sPackageInfoCache");field.setAccessible(true);
        ((android.app.PropertyInvalidatedCache<?,?>)field.get(null)).testPropertyName();
        if(args.length==3)gesher.services.SafDocumentsContractTest.restartPhase(args[1],new java.io.File(args[2]));
        else gesher.services.SafDocumentsContractTest.contracts(false);
        System.out.println("AOSP Java transport, production ATM/provider/grants and real host files; explicit Binder/Context/native initialization boundaries. No remote Binder, ART descriptor or Windows UI claim.");
    }
}
