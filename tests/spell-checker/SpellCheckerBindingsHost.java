/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import java.lang.management.ManagementFactory;
import gesher.services.SpellCheckerBindingContractTest;

public final class SpellCheckerBindingsHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        android.app.PropertyInvalidatedCache.setTestMode(true);
        var cache = android.content.pm.PackageManager.class.getDeclaredField("sPackageInfoCache");
        cache.setAccessible(true);
        ((android.app.PropertyInvalidatedCache<?, ?>)cache.get(null)).testPropertyName();
        var hour = Class.forName("com.gesher.app.locale.SystemLocale").getDeclaredField("sIs24Hour");
        hour.setAccessible(true); hour.set(null, Boolean.TRUE);
        boolean baseline = Boolean.parseBoolean(args[2]);
        if (!args[1].equals("work")) SpellCheckerBindingContractTest.contracts(false);
        if (!args[1].equals("contracts")) {
            com.sun.management.ThreadMXBean bean = (com.sun.management.ThreadMXBean) ManagementFactory.getThreadMXBean();
            bean.setThreadAllocatedMemoryEnabled(true); bean.setThreadCpuTimeEnabled(true);
            long main = Thread.currentThread().getId();
            for (int sessions : new int[]{1, 10, 100, 1000}) {
                System.out.println("SPELL_BINDING_WORK " + SpellCheckerBindingContractTest.work(sessions, !baseline,
                        worker -> new long[]{bean.getThreadAllocatedBytes(main) + bean.getThreadAllocatedBytes(worker),
                                bean.getThreadCpuTime(main) + bean.getThreadCpuTime(worker)}));
            }
            System.out.println("SPELL_BINDING_WORK " + SpellCheckerBindingContractTest.churn(1000, !baseline,
                    worker -> new long[]{bean.getThreadAllocatedBytes(main) + bean.getThreadAllocatedBytes(worker),
                            bean.getThreadCpuTime(main) + bean.getThreadCpuTime(worker)}));
        }
        System.out.println("Production session manager and ActiveServices; selected manifest metadata, provider and death delivery are explicit fixtures. Host has no Parcel/ART/Windows execution claim.");
    }
}
