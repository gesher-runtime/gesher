/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.pm.ShortcutInfo;
import android.os.PersistableBundle;
import gesher.services.ShortcutUpdateContractTest;
import java.lang.reflect.Field;
import java.util.LinkedHashMap;
import java.util.Map;
import sun.misc.Unsafe;

/** Explicit host-only publisher/storage boundary; the service update and merge bodies stay real. */
public final class ShortcutUpdateHost {
    private static LinkedHashMap<String, ShortcutInfo> stored = new LinkedHashMap<>();

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
        return null;
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
        DesktopShortcutService service = (DesktopShortcutService)((Unsafe)field.get(null))
                .allocateInstance(DesktopShortcutService.class);
        ShortcutUpdateContractTest.contracts(service, "fixture.shortcuts");
        System.out.println("Host publisher/storage adapters only; Windows fixture checks actual identity, Parcel and persistence.");
    }
}
