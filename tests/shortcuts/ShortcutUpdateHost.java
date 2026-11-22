/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.pm.ShortcutInfo;
import android.os.PersistableBundle;
import gesher.services.ShortcutUpdateContractTest;
import java.lang.reflect.Field;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.List;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import sun.misc.Unsafe;

/** Explicit host-only publisher/storage boundary; the service update and merge bodies stay real. */
public final class ShortcutUpdateHost {
    private static LinkedHashMap<String, ShortcutInfo> stored = new LinkedHashMap<>();
    private static ApkPackage owner;

    private static LinkedHashMap<String, ShortcutInfo> detached(Map<String, ShortcutInfo> source) {
        LinkedHashMap<String, ShortcutInfo> result = new LinkedHashMap<>();
        for (ShortcutInfo original : source.values()) {
            ShortcutInfo copy = original.clone(ShortcutInfo.CLONE_REMOVE_FOR_CREATOR);
            if (original.getExtras() != null) {
                try {
                    Field extras = ShortcutInfo.class.getDeclaredField("mExtras");
                    extras.setAccessible(true);
                    extras.set(copy, new PersistableBundle(original.getExtras()));
                } catch (ReflectiveOperationException failure) {
                    throw new AssertionError("Host storage cannot detach extras", failure);
                }
            }
            result.put(copy.getId(), copy);
        }
        return result;
    }
    static ApkPackage publisher(String pkg, int user) {
        if (!"fixture.shortcuts".equals(pkg) || user != 0) throw new SecurityException("host fixture publisher");
        return owner;
    }
    static LinkedHashMap<String, ShortcutInfo> load() { return detached(stored); }
    static boolean publish(Map<String, ShortcutInfo> snapshot) {
        stored = detached(snapshot);
        return true;
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Field field = Unsafe.class.getDeclaredField("theUnsafe");
        field.setAccessible(true);
        owner = (ApkPackage)((Unsafe)field.get(null)).allocateInstance(ApkPackage.class);
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting("fixture.shortcuts", "/fixture/shortcuts/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(36); parsed.hideAsFinal();
        for (var entry : Map.of("packageName", "fixture.shortcuts", "apkPath", "/fixture/shortcuts/base.apk",
                "parsed", parsed, "mUid", 10000, "mDataDir", System.getenv("ANDROID_DATA") + "/fixture.shortcuts").entrySet()) {
            Field target = ApkPackage.class.getDeclaredField(entry.getKey()); target.setAccessible(true); target.set(owner, entry.getValue());
        }
        var locales = com.gesher.app.locale.SystemLocale.class.getDeclaredField("sLocales"); locales.setAccessible(true);
        locales.set(null, new android.os.LocaleList(java.util.Locale.US));
        PackageManagerService packages = new PackageManagerService(List.of(owner), false);
        DesktopShortcutService service = new DesktopShortcutService(packages, new CallerIdentity(packages, null));
        ShortcutUpdateContractTest.contracts(service, "fixture.shortcuts");
        System.out.println("Host storage adapters and actual system-UID quota exemption; Windows fixture checks actual identity, Parcel and persistence.");
    }
}
