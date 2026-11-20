/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
public final class SafDocumentsHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        android.app.PropertyInvalidatedCache.setTestMode(true);
        var field=android.content.pm.PackageManager.class.getDeclaredField("sPackageInfoCache");field.setAccessible(true);
        ((android.app.PropertyInvalidatedCache<?,?>)field.get(null)).testPropertyName();
        if(args.length==3)gesher.services.SafDocumentsContractTest.restartPhase(args[1],new java.io.File(args[2]));
        else gesher.services.SafDocumentsContractTest.contracts(false);
        System.out.println("AOSP Java transport, production ATM/provider/grants and real host files; explicit Binder/Context/native initialization boundaries. No remote Binder, ART descriptor or Windows UI claim.");
    }
}
