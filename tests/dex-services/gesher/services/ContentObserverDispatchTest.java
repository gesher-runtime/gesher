/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.ContentResolver;
import android.content.IContentService;
import android.database.IContentObserver;
import android.net.Uri;
import android.os.IBinder;
import java.lang.reflect.Field;
import java.lang.reflect.Proxy;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;
import sun.misc.Unsafe;

/** Complete production registration/notification/removal paths, with local Binder lifetimes. */
public final class ContentObserverDispatchTest {
    public static int checks;
    private static final int SKIP = ContentResolver.NOTIFY_SKIP_NOTIFY_FOR_DESCENDANTS;

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }

    private static Field field(Class<?> owner, String name) throws Exception {
        Field field = owner.getDeclaredField(name);
        field.setAccessible(true);
        return field;
    }

    public static IContentService service() throws Exception {
        check(!com.gesher.server.GmsSignatureSpoofing.isEnabled(),
                "Dispatch fixture requires ordinary desktop policy; grants have a separate fixture");
        Unsafe unsafe = (Unsafe) field(Unsafe.class, "theUnsafe").get(null);
        Class<?> type = Class.forName("com.gesher.server.ContentService");
        Class<?> syncType = Class.forName("com.gesher.server.SyncManager");
        var constructor = type.getDeclaredConstructor(syncType);
        constructor.setAccessible(true);
        // Use all real service initializers. The unused sync/process dependency is absent.
        return (IContentService) constructor.newInstance(unsafe.allocateInstance(syncType));
    }

    private record Delivery(int id, boolean self, List<Uri> uris, int flags, int user) { }

    public static final class Observer implements IContentObserver {
        final int id;
        final List<Delivery> deliveries;
        final List<IBinder.DeathRecipient> links = new ArrayList<>();
        final IBinder binder;
        long calls, uriCount;
        Observer(int id, List<Delivery> deliveries) {
            this.id = id;
            this.deliveries = deliveries;
            binder = (IBinder) Proxy.newProxyInstance(IBinder.class.getClassLoader(),
                    new Class<?>[]{IBinder.class}, (proxy, method, args) -> {
                switch (method.getName()) {
                    case "hashCode": case "equals":
                        throw new AssertionError("Content dispatch called application Binder equality");
                    case "linkToDeath": synchronized (links) { links.add((IBinder.DeathRecipient) args[0]); } return null;
                    case "unlinkToDeath": synchronized (links) { return links.remove(args[0]); }
                    case "isBinderAlive": case "pingBinder": return true;
                    case "queryLocalInterface": return null;
                    case "toString": return "ContentDispatchObserver-" + id;
                    default: throw new UnsupportedOperationException(method.toString());
                }
            });
        }
        @Override public IBinder asBinder() { return binder; }
        @Override public void onChange(boolean self, Uri uri, int user) { onChangeEtc(self, new Uri[]{uri}, 0, user); }
        @Override public synchronized void onChangeEtc(boolean self, Uri[] uris, int flags, int user) {
            calls++;
            uriCount += uris.length;
            if (deliveries != null) deliveries.add(new Delivery(id, self, Arrays.asList(uris.clone()), flags, user));
        }
        void die() {
            List<IBinder.DeathRecipient> snapshot;
            synchronized (links) { snapshot = new ArrayList<>(links); }
            for (IBinder.DeathRecipient recipient : snapshot) recipient.binderDied();
        }
    }

    private static void add(IContentService service, Observer observer, String uri, boolean descendants)
            throws Exception {
        service.registerContentObserver(Uri.parse(uri), descendants, observer, 0, 36);
    }

    private static void expect(IContentService service, List<Delivery> received, String changed,
            int flags, Observer self, boolean wantsSelf, int... ids) throws Exception {
        received.clear();
        Uri uri = Uri.parse(changed);
        service.notifyChange(new Uri[]{uri}, self, wantsSelf, flags, 0, 36, null);
        check(received.size() == ids.length, "Wrong recipients for " + uri + " flags=" + flags + ": " + received);
        for (int i = 0; i < ids.length; i++) {
            Delivery delivery = received.get(i);
            check(delivery.equals(new Delivery(ids[i], self != null && self.id == ids[i], List.of(uri), flags, 0)),
                    "Wrong ordered delivery for " + uri + ": " + received);
        }
    }

    private static int roots(IContentService service) throws Exception {
        return ((Map<?, ?>) field(service.getClass(), "mObserverRoots").get(service)).size();
    }

    private static void indexOwnership(IContentService service) throws Exception {
        Map<?, ?> roots = (Map<?, ?>) field(service.getClass(), "mObserverRoots").get(service);
        ArrayDeque<Object> pending = new ArrayDeque<>();
        for (Object authorities : roots.values()) pending.addAll(((Map<?, ?>) authorities).values());
        int entries = 0;
        while (!pending.isEmpty()) {
            Object node = pending.removeLast();
            Class<?> type = node.getClass();
            List<?> observers = (List<?>) field(type, "observers").get(node);
            Map<?, ?> children = (Map<?, ?>) field(type, "children").get(node);
            int count = observers.size();
            entries += count;
            for (Object observer : observers) {
                check(field(observer.getClass(), "node").get(observer) == node,
                        "Observer lost its owned index node");
            }
            for (Object child : children.values()) {
                check(field(type, "parent").get(child) == node, "Index child lost its parent");
                count += field(type, "subtreeSize").getInt(child);
                pending.add(child);
            }
            check(count == field(type, "subtreeSize").getInt(node) && count > 0,
                    "Index retained an empty branch or an incorrect subtree count");
        }
        check(entries == ((List<?>) field(service.getClass(), "mObservers").get(service)).size(),
                "Index and live registry no longer agree");
    }

    private static void matching() throws Exception {
        IContentService service = service();
        List<Delivery> received = new ArrayList<>();
        Observer[] observers = new Observer[11];
        for (int i = 0; i < observers.length; i++) observers[i] = new Observer(i, received);
        add(service, observers[0], "content://alpha/items/child", false);
        add(service, observers[1], "content://alpha", true);
        add(service, observers[2], "content://alpha/items", false);
        add(service, observers[3], "content://alpha/items", true);
        add(service, observers[4], "content://alpha/items2/child", true);
        add(service, observers[5], "content://alpha/items/child", true);
        add(service, observers[6], "content://beta/items/child", true);
        add(service, observers[7], "http://alpha/items/child", true);
        add(service, observers[8], "content://alpha/a%2Fb", false);
        add(service, observers[9], "content://alpha/a/b", false);
        add(service, observers[10], "content://alpha/%D7%90", false);
        expect(service, received, "content://alpha/items/child", 0, null, false, 0, 1, 3, 5);
        expect(service, received, "content://alpha/items/child", SKIP, null, false, 0, 1, 3);
        expect(service, received, "content://alpha/items", 0, null, false, 0, 1, 2, 3, 5);
        expect(service, received, "content://alpha/items", SKIP, null, false, 0, 1, 2);
        expect(service, received, "content://alpha/items/missing", 0, null, false, 1, 3);
        expect(service, received, "content://alpha/items/missing", SKIP, null, false, 1, 3);
        expect(service, received, "content://alpha/items2/child", 0, null, false, 1, 4);
        expect(service, received, "content://alpha/items/child?query=yes#part", 0, null, false, 0, 1, 3, 5);
        expect(service, received, "content://alpha/a%2fb", 0, null, false, 1, 8);
        expect(service, received, "content://alpha/a/b", 0, null, false, 1, 9);
        expect(service, received, "content://alpha/א", 0, null, false, 1, 10);
        expect(service, received, "content://alpha", 0, null, false, 0, 1, 2, 3, 4, 5, 8, 9, 10);
        expect(service, received, "content://alpha", SKIP, null, false, 0, 2, 8, 9, 10);
        expect(service, received, "content://beta/items/child", 0, null, false, 6);
        expect(service, received, "http://alpha/items/child", 0, null, false, 7);
        expect(service, received, "content://missing/items/child", 0, null, false);
        expect(service, received, "content://alpha/items/child", 0, observers[3], false, 0, 1, 5);
        expect(service, received, "content://alpha/items/child", 0, observers[3], true, 0, 1, 3, 5);
        for (Observer observer : observers) service.unregisterContentObserver(observer);
        check(roots(service) == 0, "Unregistration retained authority/path roots");

        // Unmanaged legacy URI inputs retain their existing equality behavior.
        add(service, observers[0], "/items", true);
        add(service, observers[1], "content:/items", false);
        add(service, observers[2], "custom:opaque", true);
        expect(service, received, "/items/child", 0, null, false, 0);
        expect(service, received, "content:/items", 0, null, false, 1);
        expect(service, received, "custom:different", 0, null, false, 2);
        for (Observer observer : observers) service.unregisterContentObserver(observer);
        check(roots(service) == 0, "Null-authority URI root was retained");
    }

    private static void batching() throws Exception {
        IContentService service = service();
        List<Delivery> received = new ArrayList<>();
        Observer repeated = new Observer(0, received), later = new Observer(1, received);
        add(service, repeated, "content://alpha/items/child", false);
        add(service, later, "content://alpha", true);
        add(service, repeated, "content://alpha/items", true);
        Uri parent = Uri.parse("content://alpha/items"), child = Uri.parse("content://alpha/items/child");
        Uri[] batch = {parent, child, child};
        service.notifyChange(batch, null, false, 0, 0, 36, null);
        check(received.equals(List.of(
                new Delivery(0, false, List.of(parent, parent, child, child, child, child), 0, 0),
                new Delivery(1, false, List.of(parent, child, child), 0, 0))),
                "Batch grouping, URI multiplicity or first-seen registration order changed: " + received);
        check(Arrays.equals(batch, new Uri[]{parent, child, child}), "Notifier's URI array was modified");
        repeated.die();
        expect(service, received, child.toString(), 0, null, false, 1);
        service.unregisterContentObserver(later);
        check(repeated.links.isEmpty() && later.links.isEmpty() && roots(service) == 0,
                "Death/unregistration retained observer index or death ownership");
    }

    private static void deepPaths() throws Exception {
        IContentService service = service();
        List<Delivery> received = new ArrayList<>();
        Observer observer = new Observer(0, received);
        StringBuilder path = new StringBuilder("content://deep");
        for (int i = 0; i < 12_000; i++) path.append("/x");
        add(service, observer, path.toString(), true);
        expect(service, received, path.toString(), 0, null, false, 0);
        expect(service, received, "content://deep", 0, null, false, 0);
        service.unregisterContentObserver(observer);
        check(roots(service) == 0, "Deep path was not pruned iteratively");
    }

    private static void broadInvalidation() throws Exception {
        IContentService service = service();
        List<Delivery> received = new ArrayList<>();
        Observer[] observers = new Observer[200];
        for (int i = 0; i < observers.length; i++) {
            observers[i] = new Observer(i, received);
            add(service, observers[i], "content://" + (i < 120 ? "alpha" : "beta") + "/rows/" + i,
                    (i & 1) != 0);
        }
        indexOwnership(service);
        // The same contract crosses the broad linear fallback and narrow tree
        // selection after unrelated observer lifetimes change its density.
        for (int first : new int[]{0, 90}) {
            for (int flags : new int[]{0, SKIP}) {
                List<Integer> expected = new ArrayList<>();
                for (int i = first; i < 120; i++) if (flags == 0 || (i & 1) == 0) expected.add(i);
                expect(service, received, "content://alpha/rows", flags, null, false,
                        expected.stream().mapToInt(Integer::intValue).toArray());
            }
            if (first == 0) for (int i = 0; i < 90; i++) service.unregisterContentObserver(observers[i]);
            indexOwnership(service);
        }
        for (Observer observer : observers) service.unregisterContentObserver(observer);
        indexOwnership(service);
        check(roots(service) == 0, "Broad/narrow switch retained retired roots");
        for (Observer observer : observers) add(service, observer, "content://shared/rows", true);
        int[] ids = new int[observers.length];
        for (int i = 0; i < ids.length; i++) ids[i] = i;
        expect(service, received, "content://shared/rows/child", 0, null, false, ids);
        expect(service, received, "content://shared/rows/child", SKIP, null, false, ids);
        for (Observer observer : observers) service.unregisterContentObserver(observer);
        check(roots(service) == 0, "Shared ancestor subscriptions retained their path");

        // Every live registration is below this URI, including a mixture of
        // exact-only and descendant subscriptions. SKIP and self delivery still
        // apply when the index can prove that the URI covers the whole registry.
        for (int i = 0; i < observers.length; i++) {
            add(service, observers[i], "content://all/rows/" + i, (i & 1) != 0);
        }
        expect(service, received, "content://all/rows?query=yes#part", 0, null, false, ids);
        expect(service, received, "content://all/rows", 0, observers[0], false,
                Arrays.copyOfRange(ids, 1, ids.length));
        expect(service, received, "content://all/rows", 0, observers[0], true, ids);
        int[] exactOnly = new int[observers.length / 2];
        for (int i = 0; i < exactOnly.length; i++) exactOnly[i] = i * 2;
        expect(service, received, "content://all/rows", SKIP, null, false, exactOnly);
        add(service, observers[0], "content://all/rows/again", false);
        Observer unrelated = new Observer(200, received);
        Uri parent = Uri.parse("content://all/rows");
        for (boolean hasUnrelated : new boolean[]{false, true}) {
            if (hasUnrelated) add(service, unrelated, "content://unrelated/rows", true);
            for (int flags : new int[]{0, SKIP}) {
                for (boolean wantsSelf : new boolean[]{false, true}) {
                    received.clear();
                    service.notifyChange(new Uri[]{parent}, observers[0], wantsSelf, flags, 0, 36, null);
                    List<Delivery> expected = new ArrayList<>();
                    for (int i = 0; i < observers.length; i++) {
                        if (i == 0 && !wantsSelf || flags == SKIP && (i & 1) != 0) continue;
                        expected.add(new Delivery(i, i == 0,
                                i == 0 ? List.of(parent, parent) : List.of(parent), flags, 0));
                    }
                    check(received.equals(expected), "Complete-subtree/linear switch lost self policy, "
                            + "order or repeated Binder registrations: " + received);
                }
            }
        }
        for (Observer observer : observers) observer.die();
        unrelated.die();
        expect(service, received, "content://all/rows", 0, null, false);
        indexOwnership(service);
        check(roots(service) == 0, "Complete-subtree death retained its paths");
    }

    private static void concurrentLifetime() throws Exception {
        IContentService service = service();
        CountDownLatch start = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        List<Thread> threads = new ArrayList<>();
        for (int index = 0; index < 4; index++) {
            final int worker = index;
            Thread thread = new Thread(() -> {
                try {
                    start.await();
                    for (int cycle = 0; cycle < 150; cycle++) {
                        Observer observer = new Observer(worker, null);
                        String uri = "content://concurrent/" + worker + "/" + cycle;
                        add(service, observer, uri, true);
                        add(service, observer, uri + "/child", false);
                        service.notifyChange(new Uri[]{Uri.parse(uri)}, null, false, 0, 0, 36, null);
                        if ((cycle & 1) == 0) observer.die();
                        else service.unregisterContentObserver(observer);
                        if (observer.calls != 1 || observer.uriCount != 2 || !observer.links.isEmpty()) {
                            throw new AssertionError("Concurrent observer retained or lost callback/lifetime ownership");
                        }
                    }
                } catch (Throwable error) { failure.compareAndSet(null, error); }
            }, "content-index-lifetime-" + index);
            threads.add(thread);
            thread.start();
        }
        start.countDown();
        for (Thread thread : threads) {
            thread.join(10_000);
            check(!thread.isAlive(), "Observer lifetime worker did not retire");
        }
        if (failure.get() != null) throw new AssertionError("Concurrent observer index failure", failure.get());
        check(roots(service) == 0, "Concurrent retirement retained tree roots");
    }

    public static void contracts() throws Exception {
        matching();
        batching();
        deepPaths();
        broadInvalidation();
        concurrentLifetime();
    }

    /** Shared workload used by the host benchmark; all operations enter the real service. */
    public static final class Workload implements AutoCloseable {
        public final IContentService service;
        public final Observer[] observers;
        public final Uri[] changed;
        public final int matching;
        public Workload(int registrations, String kind) throws Exception {
            service = service();
            observers = new Observer[registrations];
            for (int i = 0; i < registrations; i++) {
                observers[i] = new Observer(i, null);
                add(service, observers[i], "ancestors".equals(kind) ? "content://bench/rows"
                        : "content://bench/rows/" + i, "ancestors".equals(kind));
            }
            matching = "broad".equals(kind) || "ancestors".equals(kind) ? registrations
                    : "miss".equals(kind) ? 0 : 1;
            changed = new Uri[]{Uri.parse("broad".equals(kind) ? "content://bench/rows"
                    : "miss".equals(kind) ? "content://absent/rows/0" : "content://bench/rows/0")};
        }
        public void run(int count) throws Exception {
            for (int i = 0; i < count; i++) service.notifyChange(changed, null, false, 0, 0, 36, null);
        }
        public long calls() { long count = 0; for (Observer o : observers) count += o.calls; return count; }
        public long uris() { long count = 0; for (Observer o : observers) count += o.uriCount; return count; }
        @Override public void close() throws Exception {
            for (Observer observer : observers) service.unregisterContentObserver(observer);
        }
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        android.system.Os.unsetenv("GESHER_MICROG_SPOOF");
        contracts();
        System.out.println("ContentObserverDispatchTest all passed: " + checks + " checks");
    }
}
