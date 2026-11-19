/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net;

import android.net.Network;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.SystemClock;
import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.NetworkInterface;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.BooleanSupplier;

/** Actual continuing worker/state path; transport seams are named and explicit. */
public final class NsdSubscriptionWorkerTest {
    static int checks;
    static final String INSTANCE = "Owned.שלום\\😀", TYPE = "_grsubscription._tcp";
    static void check(boolean value, String why) { if (!value) throw new AssertionError(why); checks++; }
    static void await(BooleanSupplier value, String why) throws Exception {
        long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(6);
        while (!value.getAsBoolean() && System.nanoTime() < end) Thread.sleep(10);
        check(value.getAsBoolean(), why);
    }
    static NsdServiceInfo selector() { return new NsdServiceInfo(INSTANCE, TYPE); }
    record Event(String kind, long scope, NsdServiceInfo info) {}
    static final class Callback implements WindowsNsdSubscriber.Callback {
        final BlockingQueue<Event> events = new LinkedBlockingQueue<>();
        final AtomicInteger ready = new AtomicInteger(), failures = new AtomicInteger();
        public void ready() { ready.incrementAndGet(); }
        public void updated(long scope, NsdServiceInfo info) { events.add(new Event("updated", scope, info)); }
        public void lost(long scope) { events.add(new Event("lost", scope, null)); }
        public void failed(int error) { failures.incrementAndGet(); events.add(new Event("failed:" + error, 0, null)); }
        Event next(String kind) throws Exception {
            Event event = events.poll(6, TimeUnit.SECONDS);
            check(event != null && kind.equals(event.kind()), "expected " + kind + ", observed " + event);
            return event;
        }
        void silent(String why) throws Exception { check(events.poll(350, TimeUnit.MILLISECONDS) == null, why); }
    }

    /** Owns only an explicit host queue, with finite ordinary retirement observations. */
    static class Driver implements WindowsNsdResolver.Driver {
        volatile long generation = 1, lifetime = 1;
        volatile boolean unstable, absent, failPoll, failSend, closed;
        volatile int closeFailures;
        final int index, family, network;
        final BlockingQueue<WindowsNsdResolver.Packet> incoming = new LinkedBlockingQueue<>();
        final AtomicInteger configured = new AtomicInteger(), sends = new AtomicInteger(), closes = new AtomicInteger();
        final AtomicInteger unknownSnapshots = new AtomicInteger();
        final CountDownLatch released = new CountDownLatch(1);
        volatile Thread owner;
        Driver() { this(7, 4, 107); }
        Driver(int index, int family, int network) { this.index = index; this.family = family; this.network = network; }
        synchronized void owned() {
            if (owner == null) owner = Thread.currentThread();
            if (owner != Thread.currentThread()) throw new AssertionError("driver escaped its owning worker");
            if (closed) throw new AssertionError("operation after actual close");
        }
        WindowsNsdResolver.Interface scope() {
            return new WindowsNsdResolver.Interface(index, family == 4 ? index : 0,
                    family == 6 ? index : 0, 9007, generation, family == 4 ? 1 : 2, network);
        }
        public WindowsNsdResolver.Interface[] interfaces() { throw new AssertionError("subscription requires actual continuity boundary"); }
        public WindowsNsdResolver.SubscriptionScope[] subscriptionInterfaces() {
            owned();
            if (unstable) { unknownSnapshots.incrementAndGet(); return null; }
            return absent ? new WindowsNsdResolver.SubscriptionScope[0]
                    : new WindowsNsdResolver.SubscriptionScope[]{new WindowsNsdResolver.SubscriptionScope(scope(), lifetime)};
        }
        public boolean configure(WindowsNsdResolver.Interface[] values) { owned(); configured.incrementAndGet(); return true; }
        public long generation() { owned(); return generation; }
        public WindowsNsdResolver.Packet poll(int timeout) throws IOException {
            owned();
            if (failPoll) throw new IOException("explicit owned transport retirement");
            try { return incoming.poll(Math.min(timeout, 50), TimeUnit.MILLISECONDS); }
            catch (InterruptedException error) { throw new IOException(error); }
        }
        public boolean send(int target, byte[] query) throws IOException {
            owned(); if (target != index) throw new AssertionError("wrong outgoing scope");
            sends.incrementAndGet(); return !failSend;
        }
        public boolean close() {
            owned(); closes.incrementAndGet();
            if (closeFailures-- > 0) return false;
            closed = true; released.countDown(); return true;
        }
        void packet(byte[] bytes) { incoming.add(new WindowsNsdResolver.Packet(generation, index, family, index, bytes)); }
    }
    static WindowsNsdSubscriber.Backend backend(WindowsNsdResolver.DriverFactory factory) {
        return WindowsNsdSubscriber.withDriver(factory, SystemClock::elapsedRealtime);
    }
    static void stop(WindowsNsdSubscriber.Backend backend, Driver driver, int id) throws Exception {
        CountDownLatch complete = new CountDownLatch(1);
        backend.unregister(id, complete::countDown);
        check(complete.await(6, TimeUnit.SECONDS) && driver.closed, "stop completes after owned driver release");
    }
    static Map<String, byte[]> fixtures(Path path) throws Exception {
        Map<String, byte[]> values = new LinkedHashMap<>();
        for (String line : Files.readAllLines(path.resolve("subscription-fixtures.tsv"))) {
            String[] pair = line.split("\t", 2); values.put(pair[0], HexFormat.of().parseHex(pair[1]));
        }
        return values;
    }
    static void transitions(Map<String, byte[]> packets) throws Exception {
        Driver driver = new Driver(); driver.unstable = true;
        Callback callback = new Callback(); WindowsNsdSubscriber.Backend backend = backend(() -> driver);
        backend.register(1, selector(), callback);
        callback.silent("unstable initial snapshot neither succeeds nor fails");
        check(callback.ready.get() == 0, "unknown continuity is not fabricated readiness");
        driver.unstable = false;
        await(() -> callback.ready.get() == 1, "genuine stable initialization establishes subscription");
        driver.packet(packets.get("complete")); Event first = callback.next("updated");
        check(first.info().getNetwork().netId == 107 && first.info().getPort() == 43123,
                "worker emits selected actual scope and SRV value");
        driver.packet(packets.get("complete")); callback.silent("identical receipt refresh remains silent");
        driver.packet(packets.get("update")); Event changed = callback.next("updated");
        check(changed.scope() == first.scope() && Arrays.equals(changed.info().getAttributes().get("bin"),
                new byte[]{0, (byte)255, (byte)254, '=', (byte)128, 2, 3}), "binary change preserves same scope lifetime");
        int before = driver.configured.get(); driver.generation++;
        await(() -> driver.configured.get() > before, "unrelated raw generation reconciles actual continuity");
        driver.packet(packets.get("update")); callback.silent("continuous scope retains value across raw generation");
        driver.unstable = true; Thread.sleep(1100);
        callback.silent("unstable later snapshot never invents a service loss"); driver.unstable = false;
        driver.generation++; driver.lifetime++;
        Event lost = callback.next("lost"); check(lost.scope() == first.scope(), "observed scope replacement loses former scope once");
        driver.packet(packets.get("reappear")); Event reappeared = callback.next("updated");
        check(reappeared.scope() != first.scope(), "replacement has a new scope event identity");
        driver.packet(packets.get("goodbye")); callback.next("lost");
        driver.packet(packets.get("reappear")); callback.next("updated");
        driver.closeFailures = 1; int closeBefore = driver.closes.get(); CountDownLatch stopped = new CountDownLatch(1);
        backend.unregister(1, stopped::countDown);
        await(() -> driver.closes.get() > closeBefore, "explicit stop starts actual retirement");
        check(stopped.getCount() == 1, "failed native close cannot report completion");
        driver.packet(packets.get("complete")); callback.silent("retiring worker suppresses late input delivery");
        check(stopped.await(6, TimeUnit.SECONDS) && driver.closed && driver.closes.get() == closeBefore + 2,
                "bounded retry completes after actual release");
        check(callback.failures.get() == 0, "established subscription never emits registration failure");
    }
    static void expireWhileUnstable(Map<String, byte[]> packets) throws Exception {
        Driver driver = new Driver(); Callback callback = new Callback();
        AtomicLong now = new AtomicLong(SystemClock.elapsedRealtime());
        WindowsNsdSubscriber.Backend backend = WindowsNsdSubscriber.withDriver(() -> driver, now::get);
        backend.register(3, selector(), callback); await(() -> callback.ready.get() == 1, "expiry fixture initialized");
        long receiptFloor = SystemClock.elapsedRealtime();
        driver.packet(packets.get("complete")); Event first = callback.next("updated");
        int observed = driver.unknownSnapshots.get(); driver.unstable = true; driver.generation++;
        await(() -> driver.unknownSnapshots.get() > observed, "unknown snapshot observed after complete response");
        // The genuine wire decoder timestamps records with SystemClock; bound
        // the observed receipt rather than assuming an arbitrary fixture epoch.
        int sends = driver.sends.get(); now.set(receiptFloor + 119_000);
        callback.silent("known 120s SRV remains live before expiry despite unknown scope");
        check(driver.sends.get() == sends, "unknown continuity does not send on stale configured scope");
        now.set(SystemClock.elapsedRealtime() + 121_000); Event lost = callback.next("lost");
        check(lost.scope() == first.scope(), "known SRV expiry loses the observation even while snapshot is unknown");
        callback.silent("expired response emits exactly one loss while continuity remains unknown");
        check(callback.failures.get() == 0 && callback.ready.get() == 1, "record expiry preserves established subscription");
        int configured = driver.configured.get(); driver.unstable = false;
        await(() -> driver.configured.get() > configured, "actual continuity becomes stable again");
        callback.silent("stable continuity cannot resurrect or lose the already expired record again");
        stop(backend, driver, 3);
    }
    static void recover(Map<String, byte[]> packets) throws Exception {
        Driver first = new Driver(), second = new Driver(); first.closeFailures = 1; first.failSend = true;
        AtomicInteger opened = new AtomicInteger(); Callback callback = new Callback();
        WindowsNsdSubscriber.Backend backend = backend(() -> opened.getAndIncrement() == 0 ? first : second);
        backend.register(2, selector(), callback); await(() -> callback.ready.get() == 1, "recovery fixture initialized");
        await(() -> first.sends.get() > 0, "genuine send refusal observed by continuing worker");
        callback.silent("send refusal invents neither service loss nor registration failure");
        first.packet(packets.get("complete")); callback.next("updated"); first.failPoll = true;
        callback.next("lost"); await(() -> first.closes.get() == 1, "retiring interrupted transport remains owned");
        check(opened.get() == 1, "replacement is not opened before failed close succeeds");
        await(() -> opened.get() == 2 && second.configured.get() != 0, "real replacement begins after retirement");
        check(first.closed, "former handle was actually closed");
        second.packet(packets.get("reappear")); callback.next("updated");
        check(callback.failures.get() == 0 && callback.ready.get() == 1, "transport recovery preserves established public request");
        stop(backend, second, 2);
    }
    static void absentAndFailure() throws Exception {
        Driver absent = new Driver(); Callback callback = new Callback();
        WindowsNsdSubscriber.Backend backend = backend(() -> absent); NsdServiceInfo selector = selector(); selector.setNetwork(new Network(999));
        backend.register(3, selector, callback); await(() -> callback.ready.get() == 1, "explicit absent network remains an active subscription");
        callback.silent("absent network has no fabricated service or terminal timeout");
        check(absent.sends.get() == 0, "explicit absent network never falls back to another interface"); stop(backend, absent, 3);
        Callback rejected = new Callback(); AtomicInteger attempts = new AtomicInteger();
        WindowsNsdSubscriber.Backend unavailable = backend(() -> { attempts.incrementAndGet(); throw new IOException("owned initial unavailable"); });
        unavailable.register(4, selector(), rejected); rejected.next("failed:" + NsdManager.FAILURE_INTERNAL_ERROR);
        check(attempts.get() == 1, "initial construction failure terminates exactly once");
        CountDownLatch done = new CountDownLatch(1); unavailable.unregister(4, done::countDown);
        check(done.await(3, TimeUnit.SECONDS), "no abandoned ownership after initial failure");
        Callback type = new Callback(); NsdServiceInfo invalidType = selector(); invalidType.setServiceType("owned");
        unavailable.register(5, invalidType, type); type.next("failed:" + NsdManager.FAILURE_BAD_PARAMETERS);
        check(attempts.get() == 1, "ordinary invalid service type rejected before allocating transport");
    }

    static final class LoopbackDriver extends Driver {
        final DatagramSocket socket; final InetSocketAddress peer;
        LoopbackDriver(InetAddress address, int index, int family, int port) throws Exception {
            super(index, family, 0); socket = new DatagramSocket(new InetSocketAddress(address, 0));
            peer = new InetSocketAddress(address, port);
        }
        @Override public boolean send(int index, byte[] query) throws IOException {
            super.send(index, query); socket.send(new DatagramPacket(query, query.length, peer)); return true;
        }
        @Override public WindowsNsdResolver.Packet poll(int timeout) throws IOException {
            owned(); socket.setSoTimeout(timeout); byte[] bytes = new byte[65535];
            DatagramPacket packet = new DatagramPacket(bytes, bytes.length);
            try { socket.receive(packet); } catch (SocketTimeoutException noPacket) { return null; }
            if (!packet.getSocketAddress().equals(peer)) return null;
            return new WindowsNsdResolver.Packet(generation, index, family, index, Arrays.copyOf(bytes, packet.getLength()));
        }
        @Override public boolean close() { boolean result = super.close(); if (result) socket.close(); return result; }
    }
    static int commandNumber;
    static void command(Path control, String mode) throws Exception {
        String serial = Integer.toString(++commandNumber);
        Path temporary = control.resolve("command-new"); Files.writeString(temporary, serial + " " + mode);
        Files.move(temporary, control.resolve("command"), java.nio.file.StandardCopyOption.REPLACE_EXISTING,
                java.nio.file.StandardCopyOption.ATOMIC_MOVE);
        long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while ((!Files.exists(control.resolve("ack")) || !Files.readString(control.resolve("ack")).equals(serial))
                && System.nanoTime() < end) Thread.sleep(20);
        check(Files.exists(control.resolve("ack")) && Files.readString(control.resolve("ack")).equals(serial), "owned peer acknowledged " + mode);
    }
    static void loopback(String[] args) throws Exception {
        Path control = Path.of(args[2]); int family = Integer.parseInt(args[5]);
        InetAddress address = InetAddress.getByName(family == 4 ? "127.0.0.1" : "::1");
        int index = NetworkInterface.getByInetAddress(address).getIndex();
        List<String> ports = Files.readAllLines(control.resolve("ready"));
        LoopbackDriver driver = new LoopbackDriver(address, index, family, Integer.parseInt(ports.get(0)));
        WindowsNsdSubscriber.Backend backend = backend(() -> driver); Callback callback = new Callback();
        NsdServiceInfo selector = new NsdServiceInfo(args[4], args[3]); selector.setInterfaceIndex(index);
        backend.register(1, selector, callback);
        try {
            Event first = callback.next("updated");
            check(first.info().getPort() == Integer.parseInt(ports.get(1)) && first.info().getNetwork() == null
                    && first.info().getInterfaceIndex() == index, "loopback record carries actual owned TCP port/interface");
            check(first.info().getHostAddresses().equals(List.of(address)), "independent peer address survives real UDP receive");
            try (Socket connection = new Socket()) {
                connection.connect(new InetSocketAddress(first.info().getHost(), first.info().getPort()), 3000);
                connection.setSoTimeout(3000);
                String banner = new String(connection.getInputStream().readNBytes(31), StandardCharsets.UTF_8);
                check(banner.equals("gesher-owned-subscription-peer\n"), "returned endpoint serves the independent owned TCP peer");
            }
            command(control, "update"); Event update = callback.next("updated");
            check(Arrays.equals(update.info().getAttributes().get("bin"), new byte[]{0, (byte)255, (byte)254, '=', (byte)128, 2, 3}), "independent update keeps exact binary TXT");
            command(control, "silent"); command(control, "goodbye"); callback.next("lost");
            command(control, "reappear"); callback.next("updated");
            check(callback.failures.get() == 0 && driver.sends.get() > 0, "continuing worker really transmitted queries without registration failure");
        } finally { stop(backend, driver, 1); command(control, "quit"); }
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        if (args[1].equals("loopback")) loopback(args);
        else {
            Map<String, byte[]> packets = fixtures(Path.of(args[1]));
            transitions(packets); expireWhileUnstable(packets); recover(packets); absentAndFailure();
        }
        System.out.println("NSD_SUBSCRIPTION_WORKER_OK checks=" + checks + " windows=false remote_binder=false native_sockets=false");
    }
}
