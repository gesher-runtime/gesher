package com.gesher.app.accessibility;

import android.view.WindowManagerGlobal;
import java.lang.reflect.Field;
import java.util.Map;

/** CI-only real global monitor + actual owner STA + real destructor join. */
public final class QueryBoundaryTest {
    private static native long nativeStart(long hwnd, int kind, long node, boolean direct);
    private static native long nativeState(long id);
    private static native long nativeGeneration(long id);
    private static native long[] nativeTiming(long id);
    private static native boolean nativeJoin(long id);
    private static native int nativeActive();
    private static native int nativeAttached();
    private static native int nativeShutdowns();
    private static native boolean nativeStalePerform(long id);
    private static void require(boolean value, String name) {
        if (!value) throw new AssertionError(name);
        System.out.println("PASS query-boundary: " + name);
        System.out.flush();
    }
    private static void requireTiming(long[] times, int kind) {
        require(times != null && times.length == 4 && times[0] > 0
                        && times[1] >= times[0] && times[2] >= times[1] && times[3] >= times[2],
                "ordered actual native dispatch/wrapper/owner-join timestamps kind=" + kind);
        long wrapperNs = times[2] - times[1];
        long postWrapperNs = times[3] - times[2];
        long queryAndCloseNs = times[3] - times[1];
        System.out.println("ACTUAL_QUERY_BOUNDARY_TIMING kind=" + kind
                + " dispatch_ns=" + (times[1] - times[0]) + " wrapper_ns=" + wrapperNs
                + " post_wrapper_close_ns=" + postWrapperNs + " query_and_close_ns=" + queryAndCloseNs);
        System.out.flush();
        require(queryAndCloseNs <= 3_500_000_000L,
                "real query plus owner join within original3s budget +500ms scheduling allowance: "
                        + queryAndCloseNs + "ns");
    }
    private static Class<?> bridge() throws Exception {
        return Class.forName("com.gesher.app.accessibility.UiaBridge");
    }
    private static boolean shutdownBridge() throws Exception {
        return Boolean.TRUE.equals(bridge().getMethod("shutdown").invoke(null));
    }
    private static boolean startBridge() throws Exception {
        java.lang.reflect.Method start = bridge().getDeclaredMethod("start");
        start.setAccessible(true);
        return Boolean.TRUE.equals(start.invoke(null));
    }
    private static boolean realMonitorBlocked() {
        for (Map.Entry<Thread, StackTraceElement[]> entry : Thread.getAllStackTraces().entrySet()) {
            if (entry.getKey().getState() != Thread.State.BLOCKED) continue;
            for (StackTraceElement frame : entry.getValue()) {
                if (frame.getClassName().equals("com.gesher.app.accessibility.UiaBridge")
                        && frame.getMethodName().equals("viewRoots")) {
                    System.out.println("ACTUAL_GLOBAL_MONITOR_WAITER thread=" + entry.getKey().getName()
                            + " state=" + entry.getKey().getState() + " frame=" + frame);
                    System.out.flush();
                    return true;
                }
            }
        }
        return false;
    }
    private static int cacheSize() throws Exception {
        Field cache = bridge().getDeclaredField("sRoots");
        cache.setAccessible(true);
        return ((Map<?, ?>) cache.get(null)).size();
    }
    public static void main(String[] args) throws Exception {
        if (!"true".equals(System.getenv("GITHUB_ACTIONS"))) throw new AssertionError("CI only");
        boolean direct = args.length > 2 && args[2].equals("unwrapped-control");
        Class.forName("com.gesher.app.Main").getMethod("bootstrap", String.class, String.class, String.class)
                .invoke(null, "QueryBoundaryTest", args[0], args[1]);
        // Initialize getInstance BEFORE the lock barrier; every bare HWND is a
        // genuine cache miss and every operation must enter real viewRoots.
        WindowManagerGlobal global = WindowManagerGlobal.getInstance();
        Field field = WindowManagerGlobal.class.getDeclaredField("mLock");
        field.setAccessible(true);
        Object lock = field.get(global);
        require(startBridge(), "real UIA bridge starts");
        java.lang.reflect.Method register = bridge().getDeclaredMethod("nativeRegisterFixture", Class.class);
        register.setAccessible(true);
        require(Boolean.TRUE.equals(register.invoke(null, QueryBoundaryTest.class)), "CI-only real HWND hook registered");
        long previousGeneration = 0;
        java.util.concurrent.atomic.AtomicBoolean shutdownOkay = new java.util.concurrent.atomic.AtomicBoolean();
        Thread shutdown = new Thread(() -> {
            try { shutdownOkay.set(shutdownBridge()); }
            catch (Exception failure) { failure.printStackTrace(); }
        }, "ActualUiaShutdown");
        Class<?> nativeWindow = Class.forName("com.gesher.app.window.NativeWindow");
        for (int kind = 0; kind < 4; kind++) {
            Object window = nativeWindow.getMethod("create", String.class, int.class, int.class)
                    .invoke(null, "QueryBoundary-" + kind, 64, 64);
            long id;
            synchronized (lock) {
                id = nativeStart((Long) nativeWindow.getMethod("getHwnd").invoke(window), kind, -1, direct);
                require(id != 0, "real fixture HWND dispatch kind=" + kind);
                long generation = nativeGeneration(id);
                require(generation > previousGeneration, "fresh HWND generation kind=" + kind);
                previousGeneration = generation;
                long deadline = System.nanoTime() + 1_500_000_000L;
                while (!realMonitorBlocked() && System.nanoTime() < deadline) Thread.sleep(2);
                require((nativeState(id) & 1) != 0 && realMonitorBlocked(),
                        "owner query reached actual held global mLock kind=" + kind);
                System.out.println("ACTUAL_CLOSE_UNDER_GLOBAL_LOCK kind=" + kind + " direct=" + direct);
                System.out.flush();
                nativeWindow.getMethod("close").invoke(window); // REAL destructor waits for the owner; lock is STILL held.
                // Sample the native clock immediately after the real join. The
                // owner's receipt excludes only pre-wrapper fixture dispatch;
                // every nanosecond from wrapper entry through close still counts.
                requireTiming(nativeTiming(id), kind);
                require((nativeState(id) & 6) == 6,
                        "actual owner joined with unavailable result before lock release kind=" + kind);
                require(nativeActive() == 1 && nativeAttached() == 1,
                        "canceled worker remains attached only until held lock releases kind=" + kind);
                if (kind == 3) {
                    shutdown.start();
                    long stopEntry = System.nanoTime() + 1_000_000_000L;
                    while (nativeShutdowns() == 0 && System.nanoTime() < stopEntry) Thread.sleep(2);
                    require(nativeShutdowns() == 1 && nativeAttached() == 1,
                            "actual shutdown starts while canceled JNI worker still holds lifetime");
                }
            }
            require(nativeJoin(id), "actual sender joined exit0 kind=" + kind);
            long drain = System.nanoTime() + 3_000_000_000L;
            while ((nativeActive() != 0 || nativeAttached() != 0) && System.nanoTime() < drain) Thread.sleep(2);
            require(nativeActive() == 0 && nativeAttached() == 0,
                    "lock release drains and detaches actual JNI worker kind=" + kind);
            require(cacheSize() == 0, "canceled cache miss publishes no late root kind=" + kind);
        }
        shutdown.join(3000);
        require(!shutdown.isAlive() && shutdownOkay.get(),
                "actual in-flight shutdown drains native and event workers after lock release");
        require(nativeActive() == 0 && nativeAttached() == 0, "shutdown actual attached workers0");
        require(!startBridge(), "terminal shutdown never restarts a dead pool");
        System.out.println("ACTUAL_QUERY_BOUNDARY_SUMMARY four-kinds=4 owner-join=4 detached=0 cache=0");
        System.out.flush();
        System.exit(0);
    }
}
