/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.view.View;
import android.view.ViewRootImpl;
import java.lang.ref.WeakReference;
import java.lang.reflect.*;
import java.util.Map;
import sun.misc.Unsafe;

/** Actual bridge cache with owner allocation/UI discovery replaced at admission. */
public final class UiaCacheHost {
    static int checks;
    static final Class<?> BRIDGE = com.gesher.app.accessibility.UiaBridge.class;
    static final Class<?> ROOT;
    static final Unsafe UNSAFE;
    static {
        try {
            ROOT = Class.forName(BRIDGE.getName() + "$Root");
            Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true);
            UNSAFE = (Unsafe) f.get(null);
        } catch (Exception e) { throw new ExceptionInInitializerError(e); }
    }
    static void check(boolean ok, String why) { if (!ok) throw new AssertionError(why); ++checks; }
    static Object call(String name, Class<?>[] types, Object... args) throws Exception {
        Method m = BRIDGE.getDeclaredMethod(name, types); m.setAccessible(true); return m.invoke(null, args);
    }
    @SuppressWarnings("unchecked")
    static Map<Long, Object> map(String name) throws Exception {
        Field f = BRIDGE.getDeclaredField(name); f.setAccessible(true); return (Map<Long, Object>) f.get(null);
    }
    static Object root(long generation) throws Exception {
        Constructor<?> c = ROOT.getDeclaredConstructors()[0]; c.setAccessible(true);
        return c.newInstance(UNSAFE.allocateInstance(ViewRootImpl.class),
                UNSAFE.allocateInstance(View.class), 7, 123L, generation);
    }
    static void put(long hwnd, Object root) throws Exception {
        try { call("cacheRoot", new Class<?>[]{long.class, ROOT}, hwnd, root); }
        catch (NoSuchMethodException baseline) { map("sRoots").put(hwnd, root); }
    }
    static WeakReference<?>[] installCollectible() throws Exception {
        Object root = root(1);
        Field v = ROOT.getDeclaredField("view"), vr = ROOT.getDeclaredField("viewRoot");
        v.setAccessible(true); vr.setAccessible(true);
        put(10, root);
        return new WeakReference<?>[]{new WeakReference<>(v.get(root)), new WeakReference<>(vr.get(root))};
    }
    static void collect(WeakReference<?>[] refs) throws Exception {
        for (int i = 0; i < 100 && (refs[0].get() != null || refs[1].get() != null); ++i) {
            System.gc(); Thread.sleep(20);
        }
        check(refs[0].get() == null && refs[1].get() == null,
                "Bridge cache retains the closed window's View/ViewRootImpl");
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        collect(installCollectible()); // Also fails on unmodified production source.
        check(call("cachedRoot", new Class<?>[]{long.class}, 10L) == null, "collected entry resolves null");
        check(map("sRoots").isEmpty(), "collected metadata pruned");
        Object old = root(1), current = root(2);
        put(20, current); Object currentEntry = map("sRoots").get(20L);
        Object snapshot = call("cachedRoot", new Class<?>[]{long.class}, 20L);
        Field view = ROOT.getDeclaredField("view"); view.setAccessible(true);
        check(snapshot != null && view.get(snapshot) == view.get(current), "live cache hit lost its owner");
        put(20, old);
        check(map("sRoots").get(20L) == currentEntry, "old generation replaced a reused HWND");
        call("forgetRoot", new Class<?>[]{long.class, ROOT}, 20L, old);
        check(map("sRoots").get(20L) == currentEntry, "old cleanup removed a new generation");
        // Simulate delayed queue delivery from an entry replaced at the same HWND.
        put(30, old); Object stale = map("sRoots").get(30L); put(30, current);
        ((java.lang.ref.Reference<?>) stale).clear(); ((java.lang.ref.Reference<?>) stale).enqueue();
        check(call("cachedRoot", new Class<?>[]{long.class}, 30L) != null, "late GC removed replacement");
        for (long hwnd = 100; hwnd < 10100; ++hwnd) put(hwnd, current);
        check(map("sRoots").size() <= 256, "root metadata unbounded");
        for (long hwnd = 0; hwnd < 10000; ++hwnd) {
            check(Boolean.TRUE.equals(call("structureDue", new Class<?>[]{long.class, long.class}, hwnd, 1000L)), "first event lost");
        }
        check(map("sLastStructure").size() <= 256, "structure metadata unbounded");
        check(Boolean.FALSE.equals(call("structureDue", new Class<?>[]{long.class, long.class}, 9999L, 1100L)), "coalescing changed");
        check(Boolean.TRUE.equals(call("structureDue", new Class<?>[]{long.class, long.class}, 9999L, 1250L)), "event deadline changed");
        java.util.concurrent.atomic.AtomicReference<Throwable> failure = new java.util.concurrent.atomic.AtomicReference<>();
        Thread[] workers = new Thread[4];
        for (int i = 0; i < workers.length; ++i) {
            final int worker = i;
            workers[i] = new Thread(() -> {
                try {
                    for (int n = 0; n < 1000; ++n) {
                        put(50000 + n, current);
                        call("cachedRoot", new Class<?>[]{long.class}, 50000L + n);
                        call("forgetRoot", new Class<?>[]{long.class, ROOT}, 50000L + n, old);
                        call("structureDue", new Class<?>[]{long.class, long.class}, 50000L + n, (long)worker);
                    }
                } catch (Throwable t) { failure.compareAndSet(null, t); }
            });
            workers[i].start();
        }
        for (Thread worker : workers) worker.join();
        check(failure.get() == null, "concurrent cache operation failed: " + failure.get());
        check(map("sRoots").size() <= 256 && map("sLastStructure").size() <= 256, "concurrent cache growth");
        System.out.println("UIA cache host passed: " + checks + " checks");
    }
}
