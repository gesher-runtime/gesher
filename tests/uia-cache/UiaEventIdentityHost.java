/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.content.Context;
import android.os.IBinder;
import android.view.View;
import android.view.ViewRootImpl;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityRecord;
import com.gesher.app.window.WindowSession;
import java.lang.ref.Reference;
import java.lang.ref.WeakReference;
import java.lang.reflect.*;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import sun.misc.Unsafe;

/** Full production event and cache paths. Owner attachment, HWND registration
 * and the native event sink are explicit host boundaries, not UIA emulation. */
public final class UiaEventIdentityHost {
    static final LinkedBlockingQueue<Long> IDS = new LinkedBlockingQueue<>();
    static final Class<?> BRIDGE = com.gesher.app.accessibility.UiaBridge.class;
    static final long ROOT_NODE = 0xffffffff00000007L;
    static int checks;
    static Unsafe unsafe;
    static Class<?> rootType;
    static Constructor<?> rootConstructor;
    static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name); f.setAccessible(true); return f;
    }
    static Method method(Class<?> type, String name, Class<?>... args) throws Exception {
        Method m = type.getDeclaredMethod(name, args); m.setAccessible(true); return m;
    }
    public static void record(long id) { IDS.add(id); }
    static IBinder token() {
        return (IBinder)Proxy.newProxyInstance(IBinder.class.getClassLoader(),
                new Class<?>[]{IBinder.class}, (o, m, a) -> {
                    if (m.getName().equals("hashCode")) return System.identityHashCode(o);
                    if (m.getName().equals("equals")) return o == a[0];
                    return null;
                });
    }
    static final class AttachedView extends View {
        IBinder token;
        private AttachedView() { super((Context)null); }
        @Override public IBinder getWindowToken() { return token; }
    }
    static Object root(IBinder token, long node, long generation) throws Exception {
        AttachedView view = (AttachedView)unsafe.allocateInstance(AttachedView.class);
        view.token = token;
        return rootConstructor.newInstance(unsafe.allocateInstance(ViewRootImpl.class),
                view, 7, node, generation);
    }
    @SuppressWarnings("unchecked")
    static void cache(long hwnd, Object root) throws Exception {
        try { method(BRIDGE, "cacheRoot", long.class, rootType).invoke(null, hwnd, root); }
        catch (NoSuchMethodException baseline) {
            ((Map<Long, Object>)field(BRIDGE, "sRoots").get(null)).put(hwnd, root);
        }
    }
    static void send(IBinder token, long node) throws Exception {
        AccessibilityEvent event = AccessibilityEvent.obtain(AccessibilityEvent.TYPE_VIEW_FOCUSED);
        field(AccessibilityRecord.class, "mSourceNodeId").setLong(event, node);
        method(BRIDGE, "onEvent", AccessibilityEvent.class, IBinder.class).invoke(null, event, token);
        event.recycle();
    }
    static long event(IBinder token, long node) throws Exception {
        send(token, node);
        Long id = IDS.poll(5, TimeUnit.SECONDS);
        if (id == null) throw new AssertionError("Expected production event delivery");
        return id;
    }
    static WeakReference<?>[] collectible() throws Exception {
        IBinder token = token();
        Object root = root(token, ROOT_NODE + 9, 9);
        WindowSession.get().bind(token, 999);
        cache(999, root);
        check(event(token, ROOT_NODE + 9) == -1, "temporary root identity");
        WindowSession.get().remove(token);
        return new WeakReference<?>[]{new WeakReference<>(token),
                new WeakReference<>(field(rootType, "view").get(root)),
                new WeakReference<>(field(rootType, "viewRoot").get(root))};
    }
    static int identities() throws Exception {
        return ((Map<?, ?>)field(BRIDGE, "sRootIdentities").get(null)).size();
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        unsafe = (Unsafe)field(Unsafe.class, "theUnsafe").get(null);
        rootType = Class.forName(BRIDGE.getName() + "$Root");
        rootConstructor = rootType.getDeclaredConstructors()[0]; rootConstructor.setAccessible(true);
        field(BRIDGE, "sLoaded").set(null, true);
        IBinder first = token();
        Object firstRoot = root(first, ROOT_NODE, 1);
        WindowSession.get().bind(first, 777); cache(777, firstRoot);
        check(event(first, ROOT_NODE) == -1, "root event before eviction");
        List<Object> otherRoots = new ArrayList<>();
        for (long hwnd = 1000; hwnd < 1256; hwnd++) {
            Object other = root(token(), ROOT_NODE + hwnd, 1);
            otherRoots.add(other); cache(hwnd, other);
        }
        check(event(first, ROOT_NODE) == -1, "live root identity changed after payload eviction");
        check(event(first, ROOT_NODE + 1) == ROOT_NODE + 1, "descendant was normalized as root");
        IBinder replacement = token();
        Object newer = root(replacement, ROOT_NODE + 2, 2);
        WindowSession.get().remove(first); WindowSession.get().bind(replacement, 777);
        cache(777, newer);
        check(event(replacement, ROOT_NODE + 2) == -1, "reused HWND new token root");
        check(event(replacement, ROOT_NODE) == ROOT_NODE, "old HWND identity affected new token");
        send(first, ROOT_NODE);
        check(IDS.poll(50, TimeUnit.MILLISECONDS) == null, "unregistered token emitted an event");
        Object older = root(replacement, ROOT_NODE + 3, 1);
        method(BRIDGE, "rememberIdentity", IBinder.class, long.class, rootType)
                .invoke(null, replacement, 777L, older);
        check(event(replacement, ROOT_NODE + 2) == -1, "late generation replaced current identity");
        WindowSession.get().bind(replacement, 888);
        check(event(replacement, ROOT_NODE + 2) == ROOT_NODE + 2, "stale HWND metadata was reused");
        cache(888, newer);
        check(event(replacement, ROOT_NODE + 2) == -1, "new HWND identity was not learned");
        int retained = identities();
        WeakReference<?>[] collected = collectible();
        for (int n = 0; n < 100 && java.util.Arrays.stream(collected).anyMatch(r -> r.get() != null); n++) {
            System.gc(); Thread.sleep(20);
        }
        check(java.util.Arrays.stream(collected).allMatch(r -> r.get() == null),
                "identity metadata retained a closed token or view tree");
        check(event(replacement, ROOT_NODE + 2) == -1, "live identity lost during reclamation");
        check(identities() == retained, "collected token identity was not pruned");
        Reference.reachabilityFence(firstRoot);
        Reference.reachabilityFence(otherRoots);
        Reference.reachabilityFence(newer);
        System.out.println("UIA event identity host passed: " + checks + " checks");
    }
}
