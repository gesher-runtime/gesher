/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.os.IBinder;
import com.gesher.server.ActivityManagerService;
import com.gesher.server.ActivityTaskManagerService;
import com.gesher.server.ClipboardService;
import com.gesher.server.PackageManagerService;
import com.gesher.server.Services;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;

/** Owns the real service-context dependencies used by isolated contract fixtures. */
final class FixtureServiceContext {
    private FixtureServiceContext() { }

    private static Field field(Class<?> type, String name) throws Exception {
        Field result = type.getDeclaredField(name);
        result.setAccessible(true);
        return result;
    }

    private static List<?> observers(PackageManagerService pm, String name) throws Exception {
        synchronized (pm) {
            return new ArrayList<>((Collection<?>)field(PackageManagerService.class, name).get(pm));
        }
    }

    private static Object added(List<?> before, List<?> after, String name) {
        List<Object> additions = new ArrayList<>(after);
        additions.removeAll(before);
        if (additions.size() != 1 || !after.containsAll(before)) {
            throw new AssertionError("Real clipboard must own exactly one " + name + " observer");
        }
        return additions.get(0);
    }

    private record Owned(PackageManagerService pm, ClipboardService clipboard,
            Object change, Object removal) {
        void close() throws Exception {
            clipboard.close();
            if (observers(pm, "mPackageChanges").contains(change)
                    || observers(pm, "mPackageRemoval").contains(removal)) {
                throw new AssertionError("Fixture clipboard retained a package observer after close");
            }
        }
    }

    /** One test entry owns every context it constructs, including restart generations. */
    static final class Scope implements AutoCloseable {
        private final Field last = field(Services.class, "sLast");
        private final Object previous = last.get(null);
        private final List<Owned> owned = new ArrayList<>();
        private int metadataOnly;
        private boolean closed;

        Scope() throws Exception { }

        Object runtime(Object pm, Object atm, Object am) throws Exception {
            // These tests call their real controllers directly and publish no named binders.
            return runtime(pm, atm, am, Map.of());
        }

        Object runtime(Object pmValue, Object atmValue, Object amValue,
                Map<String, IBinder> publishedBindings) throws Exception {
            if (closed) throw new IllegalStateException("Fixture service scope is closed");
            PackageManagerService pm = (PackageManagerService)Objects.requireNonNull(pmValue);
            ActivityTaskManagerService atm = (ActivityTaskManagerService)Objects.requireNonNull(atmValue);
            ActivityManagerService am = (ActivityManagerService)Objects.requireNonNull(amValue);
            Map<String, IBinder> bindings = new LinkedHashMap<>(publishedBindings);
            bindings.forEach((name, binder) -> { Objects.requireNonNull(name); Objects.requireNonNull(binder); });
            // A lazy production AppOps ledger can register its own package observer.
            // Complete that dependency before identifying the clipboard's two subscriptions.
            pm.appOps();
            List<?> changes = observers(pm, "mPackageChanges");
            List<?> removals = observers(pm, "mPackageRemoval");
            ClipboardService clipboard = new ClipboardService(atm, pm);
            try {
                Owned owner = new Owned(pm, clipboard,
                        added(changes, observers(pm, "mPackageChanges"), "package-change"),
                        added(removals, observers(pm, "mPackageRemoval"), "package-removal"));
                owned.add(owner);
            } catch (Throwable failure) {
                clipboard.close();
                throw failure;
            }
            return created(pm, atm, am, clipboard, bindings);
        }

        /** NotificationStore's creator-identity check has no activity or activation context. */
        Object metadataOnly(Object pm) throws Exception {
            if (closed) throw new IllegalStateException("Fixture service scope is closed");
            metadataOnly++;
            return created((PackageManagerService)Objects.requireNonNull(pm), null, null, null, Map.of());
        }

        private Object created(PackageManagerService pm, ActivityTaskManagerService atm,
                ActivityManagerService am, ClipboardService clipboard, Map<String, IBinder> bindings)
                throws Exception {
            Constructor<?> constructor = Class.forName("com.gesher.server.Services$Created")
                    .getDeclaredConstructor(PackageManagerService.class, ActivityTaskManagerService.class,
                            ActivityManagerService.class, ClipboardService.class, Map.class);
            constructor.setAccessible(true);
            return constructor.newInstance(pm, atm, am, clipboard, bindings);
        }

        @Override public void close() throws Exception {
            if (closed) return;
            closed = true;
            Throwable failure = null;
            for (int i = owned.size() - 1; i >= 0; i--) {
                try { owned.get(i).close(); }
                catch (Throwable cleanup) {
                    if (failure == null) failure = cleanup; else failure.addSuppressed(cleanup);
                }
            }
            try { last.set(null, previous); }
            catch (Throwable restore) {
                if (failure == null) failure = restore; else failure.addSuppressed(restore);
            }
            if (failure instanceof Exception exception) throw exception;
            if (failure instanceof Error error) throw error;
            System.out.println("FixtureServiceContext: closed " + owned.size()
                    + " real clipboard contexts and observer pairs; metadata-only=" + metadataOnly
                    + "; restored prior Services.sLast");
        }
    }
}
