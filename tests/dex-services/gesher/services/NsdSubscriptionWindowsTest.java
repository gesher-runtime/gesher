/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.Network;
import android.net.nsd.NsdServiceInfo;
import com.gesher.net.WindowsNsdResolver;
import com.gesher.net.WindowsNsdResolver.Interface;
import com.gesher.net.WindowsNsdResolver.SubscriptionScope;
import com.gesher.net.WindowsNsdSubscriber;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.lang.reflect.Constructor;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;

/** Real Windows subscriber transport; installed-app and remote-Binder gates are separate. */
public final class NsdSubscriptionWindowsTest {
    private static int checks, command;
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private static void await(BooleanSupplier condition, String message) throws Exception {
        long deadline = System.nanoTime() + 10_000_000_000L;
        while (!condition.getAsBoolean()) {
            if (System.nanoTime() >= deadline) throw new AssertionError("Fixture deadline: " + message);
            Thread.sleep(10);
        }
        check(true, message);
    }
    private static void retired(WindowsNsdSubscriber.Backend backend) throws Exception {
        await(() -> Arrays.equals(backend.nativeState(), new int[6]),
                "all requests, sockets, watches, callbacks and progress owners released");
    }
    private static void peer(Path control, String mode) throws Exception {
        String serial = Integer.toString(++command);
        Path tmp = control.resolve("command-new");
        Files.writeString(tmp, serial + " " + mode, StandardCharsets.UTF_8);
        Files.move(tmp, control.resolve("command"), StandardCopyOption.ATOMIC_MOVE,
                StandardCopyOption.REPLACE_EXISTING);
        await(() -> {
            try { return Files.readString(control.resolve("ack")).trim().equals(serial); }
            catch (Exception pending) { return false; }
        }, "owned peer " + mode);
    }
    private static final class Event {
        final String kind; final long scope; final NsdServiceInfo value;
        Event(String kind, long scope, NsdServiceInfo value) {
            this.kind = kind; this.scope = scope; this.value = value;
        }
    }
    private static final class Listener implements WindowsNsdSubscriber.Callback {
        final BlockingQueue<Event> events = new LinkedBlockingQueue<>();
        final CountDownLatch ready = new CountDownLatch(1);
        public void ready() { ready.countDown(); }
        public void updated(long id, NsdServiceInfo value) { events.add(new Event("updated", id, value)); }
        public void lost(long id) { events.add(new Event("lost", id, null)); }
        public void failed(int error) { events.add(new Event("failed:" + error, 0, null)); }
        Event next(String kind) throws Exception {
            Event value = events.poll(15, TimeUnit.SECONDS);
            check(value != null && value.kind.equals(kind), "expected " + kind + ", got "
                    + (value == null ? "deadline" : value.kind));
            return value;
        }
        void silent(String message) throws Exception { check(events.poll(300, TimeUnit.MILLISECONDS) == null, message); }
    }
    private static NsdServiceInfo request(String type, String name, Interface scope) {
        NsdServiceInfo value = new NsdServiceInfo();
        value.setServiceType(type); value.setServiceName(name);
        if (scope.netId != 0) value.setNetwork(new Network(scope.netId));
        else value.setInterfaceIndex(scope.index);
        return value;
    }
    private static void stop(WindowsNsdSubscriber.Backend backend, int id) throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        backend.unregister(id, done::countDown);
        check(done.await(10, TimeUnit.SECONDS), "stop completed after native retirement");
        retired(backend);
    }
    public static void main(String[] args) throws Exception {
        if (args.length != 7) throw new IllegalArgumentException("type instance canonical-index family-index control family address");
        System.loadLibrary("android_runtime"); System.loadLibrary("gesher_nsd_resolve");
        String type = args[0], name = args[1];
        int index = Integer.parseInt(args[2]), familyIndex = Integer.parseInt(args[3]);
        Path control = Path.of(args[4]); int family = Integer.parseInt(args[5]);
        if (!type.matches("_gr[0-9a-f]{12}\\._tcp") || (family != 4 && family != 6)) {
            throw new IllegalArgumentException("Owned random type and actual address family required");
        }
        WindowsNsdSubscriber.Backend backend = WindowsNsdSubscriber.create(); retired(backend);
        Class<?> nativeType = Class.forName("com.gesher.net.WindowsNsdResolver$NativeSubscriptionDriver");
        Constructor<?> constructor = nativeType.getDeclaredConstructor(); constructor.setAccessible(true);
        WindowsNsdResolver.Driver inventory = (WindowsNsdResolver.Driver) constructor.newInstance();
        SubscriptionScope selected = null;
        try {
            long deadline = System.nanoTime() + 8_000_000_000L;
            while (selected == null && System.nanoTime() < deadline) {
                SubscriptionScope[] scopes = inventory.subscriptionInterfaces();
                if (scopes != null) for (SubscriptionScope scope : scopes) {
                    if (scope.scope.index == index) selected = scope;
                }
                if (selected == null) Thread.sleep(10);
            }
        } finally { await(inventory::close, "actual inventory owner retired"); }
        retired(backend);
        check(selected != null && selected.scope.luid != 0 && selected.lifetime > 0,
                "actual selected adapter has an observed lifetime");
        Interface scope = selected.scope;
        check((family == 4 ? scope.ipv4Index : scope.ipv6Index) == familyIndex, "actual family index");
        List<String> ready = Files.readAllLines(control.resolve("ready"));
        InetAddress address = InetAddress.getByName(args[6].split("%", 2)[0]);
        Listener listener = new Listener();
        try {
            backend.register(1, request(type, name, scope), listener);
            check(listener.ready.await(10, TimeUnit.SECONDS), "real transport initialized");
            Event first = listener.next("updated"); NsdServiceInfo value = first.value;
            check(value.getServiceName().equals(name) && value.getServiceType().equals(type), "named subscription identity and bare type");
            check(value.getHostname().equals("subscriber-owned") && value.getPort() == Integer.parseInt(ready.get(1)), "actual peer hostname and port");
            check(value.getHostAddresses().size() == 1
                    && Arrays.equals(value.getHostAddresses().get(0).getAddress(), address.getAddress()), "actual peer address");
            if (scope.netId != 0) check(new Network(scope.netId).equals(value.getNetwork()), "actual mapped Network");
            else check(value.getNetwork() == null && value.getInterfaceIndex() == scope.index, "actual unmapped interface");
            if (family == 6 && address.isLinkLocalAddress()) {
                check(((Inet6Address) value.getHostAddresses().get(0)).getScopeId() == scope.ipv6Index, "actual IPv6 scope");
            }
            check(value.getAttributes().containsKey("flag") && value.getAttributes().get("flag") == null, "key-only TXT");
            check(value.getAttributes().containsKey("empty") && value.getAttributes().get("empty").length == 0, "empty TXT");
            byte[] initial = new byte[205]; initial[1] = -1; initial[2] = -2; initial[3] = '='; initial[4] = -128;
            for (int at = 0; at < 200; ++at) initial[at + 5] = (byte) at;
            check(Arrays.equals(initial, value.getAttributes().get("bin")), "initial opaque binary TXT");
            try (Socket socket = new Socket()) {
                socket.connect(new InetSocketAddress(value.getHostAddresses().get(0), value.getPort()), 3000);
                socket.setSoTimeout(3000);
                check("gesher-owned-subscription-peer".equals(new BufferedReader(new InputStreamReader(
                        socket.getInputStream(), StandardCharsets.US_ASCII)).readLine()), "actual independent TCP endpoint");
            }
            await(() -> { int[] s = backend.nativeState(); return s[0] == 1 && s[1] > 0 && s[2] == 2 && s[4] == 1 && s[5] > 0; },
                    "actual sockets, watches, polling and sent datagrams");
            peer(control, "complete"); listener.silent("identical refresh did not publish a duplicate update");
            peer(control, "update"); Event changed = listener.next("updated");
            check(changed.scope == first.scope && Arrays.equals(changed.value.getAttributes().get("bin"),
                    new byte[] {0, -1, -2, '=', -128, 2, 3}), "binary update keeps observed scope");
            peer(control, "goodbye"); check(listener.next("lost").scope == first.scope, "owned goodbye withdrew observed service");
            peer(control, "reappear"); check(listener.next("updated").scope == first.scope, "owned reappearance in same interface lifetime");
            stop(backend, 1);
            peer(control, "complete"); listener.silent("late announcement after native closure is silent");
            Listener absent = new Listener(); NsdServiceInfo unavailable = request(type, name, scope);
            unavailable.setNetwork(new Network(Integer.MAX_VALUE));
            backend.register(2, unavailable, absent);
            check(absent.ready.await(10, TimeUnit.SECONDS), "absent Network completed actual observation");
            await(() -> { int[] s = backend.nativeState(); return s[0] == 1 && s[2] == 2 && s[4] == 1; }, "absent Network owner polled with real watches");
            int[] state = backend.nativeState();
            check(state[1] == 0 && state[5] == 0, "absent Network opens no fallback sockets or sends");
            absent.silent("absent Network does not fabricate an update or failure"); stop(backend, 2);
            peer(control, "quit");
            System.out.println("NsdSubscriptionWindowsTest all passed: " + checks + "; family=" + family
                    + "; production=Winsock-raw-mDNS; nativeActive=0; ordinaryApkIdentity=false; remoteBinder=false");
        } finally { backend.withdraw(1); backend.withdraw(2); }
    }
}
