/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import com.gesher.net.WindowsNsd;
import java.io.IOException;
import java.lang.ref.WeakReference;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;

/** Deterministic native-boundary fixtures run the production browse state machine. */
public final class NsdBrowseTransportTest {
    private static int checks;
    private static void check(boolean ok, String message) { if (!ok) throw new AssertionError(message); checks++; }
    private static void until(BooleanSupplier ready, String description) throws Exception {
        long deadline = System.nanoTime() + 4_000_000_000L;
        while (!ready.getAsBoolean()) {
            if (System.nanoTime() >= deadline) throw new AssertionError("Timeout " + description);
            Thread.sleep(2);
        }
        checks++;
    }
    private static final class Driver implements WindowsNsd.Driver {
        final Map<Long, ArrayDeque<String[]>> events = new HashMap<>();
        final Map<Integer, Long> indices = new HashMap<>();
        final java.util.Set<Long> baseObservers = new java.util.HashSet<>();
        final java.util.Set<Long> unsettled = new java.util.HashSet<>();
        long next = 1;
        boolean supported = true;
        volatile boolean acknowledge = true;
        volatile int cancelFailures;
        volatile int cancelCalls;
        volatile int releases;
        volatile int starts;
        long startError;
        int baseStartError;
        private RuntimeException pollError;
        String query;
        public boolean supported() { return supported; }
        public int[] interfaces() { return new int[] {7}; }
        public synchronized long start(String name, int index) {
            starts++; query = name;
            if (startError != 0) return startError;
            long id = next++; events.put(id, new ArrayDeque<>()); indices.put(index, id); return id;
        }
        public synchronized int attachBaseLoss(long id) {
            if (!events.containsKey(id)) throw new AssertionError("Attach after release");
            if (baseStartError != 0) return baseStartError;
            if (!baseObservers.add(id)) throw new AssertionError("Duplicate base observer");
            return 0;
        }
        public synchronized String[] poll(long id, int timeout) {
            RuntimeException failure = pollError;
            pollError = null;
            if (failure != null) throw failure;
            ArrayDeque<String[]> queue = events.get(id);
            if (queue == null) throw new AssertionError("Polling released request");
            String[] event = queue.poll();
            if (event == null) unsettled.remove(id);
            if (event == null) try { wait(2); } catch (InterruptedException e) { throw new AssertionError(e); }
            return event;
        }
        synchronized void failNextPoll(RuntimeException failure) {
            pollError = failure;
            notifyAll();
        }
        public synchronized int cancel(long id) {
            cancelCalls++;
            if (!events.containsKey(id)) throw new AssertionError("Cancel after release");
            if (cancelFailures-- > 0) return 5;
            if (acknowledge) events.get(id).add(new String[] {"1223", "", "0", "0", "-1", "0", "12"});
            notifyAll(); return 0;
        }
        public synchronized boolean release(long id) {
            if (events.remove(id) == null) throw new AssertionError("Double release");
            indices.values().remove(id); baseObservers.remove(id); releases++; return true;
        }
        synchronized void event(int index, int status, String name, long ttl) {
            event(index, status, name, ttl, 0, ttl, ttl == 0);
        }
        synchronized void event(int index, int status, String name, long ttl,
                long queuedMillis, long serviceTtl, boolean goodbye) {
            Long id = indices.get(index); if (id == null) throw new AssertionError("No interface " + index);
            unsettled.add(id);
            events.get(id).add(new String[] {"" + status, name, "" + ttl,
                    "" + queuedMillis, "" + serviceTtl, goodbye ? "1" : "0", "12"}); notifyAll();
        }
        synchronized void service(int index, String name, long ttl, long queuedMillis) {
            Long id = indices.get(index); if (id == null) throw new AssertionError("No interface " + index);
            unsettled.add(id);
            events.get(id).add(new String[] {"0", name, "" + ttl, "" + queuedMillis,
                    "" + ttl, ttl == 0 ? "1" : "0", "33"}); notifyAll();
        }
        synchronized int active() { return events.size(); }
        synchronized int activeBaseObservers() { return baseObservers.size(); }
        synchronized boolean settled() { return unsettled.isEmpty(); }
    }
    private static class Callbacks implements WindowsNsd.Callback {
        final List<String> events = new ArrayList<>();
        volatile long expiration;
        volatile String subtype;
        volatile long lostExpiration;
        public synchronized void started() { events.add("started"); }
        public synchronized void found(String name, String type, int index) { events.add("found " + index + " " + name + " " + type); }
        public void found(String name, String type, int index, long expiry, String observedSubtype) {
            expiration = expiry; subtype = observedSubtype; found(name, type, index);
        }
        public synchronized void lost(String name, String type, int index) { events.add("lost " + index + " " + name + " " + type); }
        public void lost(String name, String type, int index, long expiry, String observedSubtype) {
            lostExpiration = expiry; lost(name, type, index);
        }
        public synchronized void stopped() { events.add("stopped"); }
        public synchronized void failed(int error) { events.add("failed " + error); }
        synchronized long count(String prefix) { return events.stream().filter(e -> e.startsWith(prefix)).count(); }
        synchronized List<String> copy() { return new ArrayList<>(events); }
    }
    private static Thread thread(WindowsNsd.Browser browser) {
        Thread t = new Thread(browser, "Nsd-fixture"); t.setDaemon(true); t.start(); return t;
    }
    private static void finish(Thread thread) throws Exception {
        thread.join(5000); check(!thread.isAlive(), "Worker terminates");
    }
    private static final class OwnerProbe {
        final Driver driver = new Driver();
        final AtomicInteger failures = new AtomicInteger();
        WeakReference<Object> owner;
        Thread worker;
    }
    private static OwnerProbe retainingNativeRequest(AtomicLong clock) {
        OwnerProbe probe = new OwnerProbe();
        probe.driver.cancelFailures = 100;
        Object owner = new Object();
        probe.owner = new WeakReference<>(owner);
        WindowsNsd.Scope scope = () -> { owner.hashCode(); return new int[] {7}; };
        WindowsNsd.Callback callback = new Callbacks() {
            @Override public void failed(int error) { owner.hashCode(); probe.failures.incrementAndGet(); }
        };
        WindowsNsd.Browser browser = new WindowsNsd.Browser(probe.driver, "_gesher._tcp", null,
                scope, callback, clock::get);
        probe.worker = thread(browser);
        return probe;
    }
    public static void main(String[] args) throws Exception {
        AtomicLong clock = new AtomicLong(10000);
        Driver absent = new Driver(); absent.supported = false; Callbacks ac = new Callbacks();
        new WindowsNsd.Browser(absent, "_gesher._tcp", null, absent::interfaces, ac, clock::get).run();
        check(ac.copy().equals(List.of("failed 0")), "Absent API cannot start an empty successful discovery");
        check(absent.starts == 0, "Unsupported API never creates a request");
        Driver failed = new Driver(); failed.startError = -50; Callbacks fc = new Callbacks();
        new WindowsNsd.Browser(failed, "_gesher._tcp", null, failed::interfaces, fc, clock::get).run();
        check(fc.copy().equals(List.of("failed 0")), "Synchronous Windows error is a start failure");
        check(failed.releases == 0, "No cancellation of unaccepted request");
        Driver limited = new Driver(); limited.startError = -56; Callbacks lc = new Callbacks();
        new WindowsNsd.Browser(limited, "_gesher._tcp", null, limited::interfaces, lc, clock::get).run();
        check(lc.copy().equals(List.of("failed 4")), "Global native admission limit preserves max-limit error");
        for (String invalid : new String[] {"_gesher._tcp.local", "_gesher._tcp,_one,_two", "not-a-type"}) {
            Driver d = new Driver(); Callbacks c = new Callbacks();
            new WindowsNsd.Browser(d, invalid, null, d::interfaces, c, clock::get).run();
            check(c.copy().equals(List.of("failed 0")) && d.starts == 0, "Invalid/unsupported type is explicit failure");
        }
        Driver badSubtype = new Driver(); Callbacks sc = new Callbacks();
        new WindowsNsd.Browser(badSubtype, "_gesher._tcp", "_bad.dot", badSubtype::interfaces, sc, clock::get).run();
        check(sc.copy().equals(List.of("failed 6")), "Invalid subtype preserves Android bad-parameters result");
        Driver driver = new Driver(); Callbacks cb = new Callbacks();
        final int[][] selected = {new int[] {7, 11}};
        WindowsNsd.Browser browser = new WindowsNsd.Browser(driver, "_gesher._tcp.", "_printer",
                () -> selected[0], cb, clock::get, () -> 2000000L);
        Thread worker = thread(browser);
        until(() -> cb.count("started") == 1, "start");
        check(driver.query.equals("_printer._sub._gesher._tcp.local"), "Actual subtype DNS query");
        check(driver.active() == 2 && driver.activeBaseObservers() == 2,
                "One logical request and base observer per interface");
        String service = "Owned.שלום._gesher._tcp.local.";
        driver.event(7, 0, service, 3); driver.event(11, 0, service, 3);
        until(() -> cb.count("found") == 2, "both interfaces");
        check(cb.copy().contains("found 7 Owned.שלום _gesher._tcp."), "Unicode and literal period preserved");
        check(cb.expiration == 2003000L && "_printer".equals(cb.subtype), "Observed SRV TTL and query subtype provenance");
        driver.event(7, 0, service, 5);
        driver.event(7, 0, "other._unrelated._tcp.local", 50);
        until(driver::settled, "refresh consumed before advancing clock");
        clock.set(14000);
        until(() -> cb.count("lost 11") == 1, "TTL expiry isolated by interface");
        check(cb.count("found") == 2, "PTR refresh does not duplicate found");
        check(cb.count("lost 7") == 0, "Refresh preserves live service");
        driver.event(7, 0, service, 0, 0, -1, false);
        until(driver::settled, "subtype-only goodbye consumed");
        check(cb.count("lost 7") == 0, "Subtype-only goodbye does not announce a whole-service loss");
        driver.event(7, 0, service, 0);
        until(() -> cb.count("lost 7") == 1, "goodbye immediately removes old snapshot");
        check(cb.lostExpiration == 2005000L, "Goodbye retains previous SRV expiration");
        driver.event(7, 0, service, 5);
        until(() -> cb.count("found 7") == 2, "positive response after goodbye is a new discovery");
        clock.set(15200); Thread.sleep(30);
        check(cb.count("lost 7") == 1, "New positive record remains live");
        selected[0] = new int[] {11}; clock.set(17000);
        until(() -> cb.count("lost 7") == 2 && driver.active() == 1, "network removal");
        check(driver.releases == 1, "Retired interface waits for cancellation then releases");
        driver.acknowledge = false;
        browser.stop();
        until(() -> driver.cancelCalls >= 2, "stop requested");
        check(cb.count("stopped") == 0 && driver.active() == 1, "Stop flag cannot pretend native completion");
        driver.event(11, 1223, "", 0);
        finish(worker);
        check(cb.count("stopped") == 1 && driver.active() == 0, "Cancellation completion releases all resources once");
        check(cb.count("failed") == 0, "No fake failure during normal lifecycle");
        Driver partial = new Driver(); partial.baseStartError = 50;
        Callbacks partialCallbacks = new Callbacks();
        new WindowsNsd.Browser(partial, "_gesher._tcp", "_printer", partial::interfaces,
                partialCallbacks, clock::get).run();
        check(partialCallbacks.copy().equals(List.of("failed 0")), "A failed base attach never publishes discovery");
        check(partial.active() == 0 && partial.releases == 1, "Partial start releases the accepted query");
        Driver blockedAttach = new Driver(); blockedAttach.baseStartError = 56; blockedAttach.cancelFailures = 100;
        Callbacks bac = new Callbacks();
        WindowsNsd.Browser bab = new WindowsNsd.Browser(blockedAttach, "_gesher._tcp", "_printer",
                blockedAttach::interfaces, bac, clock::get);
        Thread bat = thread(bab);
        until(() -> bac.count("failed 4") == 1 && blockedAttach.cancelCalls == 1,
                "companion start failure with failed rollback");
        check(bac.count("started") == 0 && blockedAttach.active() == 1 && blockedAttach.releases == 0,
                "Failed companion start preserves ownership of the accepted primary without success");
        Thread.sleep(100);
        check(blockedAttach.cancelCalls == 1 && bac.count("failed") == 1 && bat.isAlive(),
                "Partial-start rollback backs off without duplicate callbacks or premature release");
        blockedAttach.cancelFailures = 0; finish(bat);
        check(blockedAttach.active() == 0 && blockedAttach.releases == 1 && bac.count("stopped") == 0,
                "Later rollback success releases the accepted operation once");
        Driver retry = new Driver(); retry.cancelFailures = 2; Callbacks rc = new Callbacks();
        WindowsNsd.Browser rb = new WindowsNsd.Browser(retry, "_gesher._udp", null, retry::interfaces, rc, clock::get);
        Thread rt = thread(rb); until(() -> rc.count("started") == 1, "retry start"); rb.stop(); finish(rt);
        check(retry.cancelCalls == 3 && retry.releases == 1 && rc.count("failed 0") == 1
                && rc.count("stopped") == 0, "Cancellation failure terminates listener once while native cleanup retries");
        Driver overflow = new Driver(); Callbacks oc = new Callbacks();
        WindowsNsd.Browser ob = new WindowsNsd.Browser(overflow, "_gesher._tcp", null, overflow::interfaces, oc, clock::get);
        Thread ot = thread(ob); until(() -> oc.count("started") == 1, "overflow start"); overflow.event(7, 8, "", 0); finish(ot);
        check(oc.count("failed 4") == 1 && overflow.active() == 0, "Overflow terminates explicitly after release");
        Driver broken = new Driver(); Callbacks brokenCallback = new Callbacks();
        WindowsNsd.Browser brokenBrowser = new WindowsNsd.Browser(broken, "_gesher._tcp", null,
                broken::interfaces, brokenCallback, clock::get);
        Thread brokenThread = thread(brokenBrowser);
        until(() -> brokenCallback.count("started") == 1, "backend exception start");
        broken.failNextPoll(new IllegalStateException("native transport boundary"));
        finish(brokenThread);
        check(broken.active() == 0 && brokenCallback.count("failed 0") == 1,
                "Backend exception retires listener and native resources");
        Driver blocked = new Driver(); blocked.cancelFailures = 100; Callbacks bc = new Callbacks();
        WindowsNsd.Browser bb = new WindowsNsd.Browser(blocked, "_gesher._tcp", null, blocked::interfaces, bc, clock::get);
        Thread bt = thread(bb); until(() -> bc.count("started") == 1, "blocked start");
        blocked.event(7, 8, "", 0);
        until(() -> bc.count("failed 4") == 1 && blocked.cancelCalls == 1, "failure delivered before native cleanup");
        check(blocked.active() == 1 && bt.isAlive(), "Failed Stop retains native request safely");
        Thread.sleep(100);
        check(blocked.cancelCalls == 1 && bc.count("failed") == 1, "Stop failures neither spin nor duplicate original failure");
        blocked.cancelFailures = 0; finish(bt);
        check(blocked.active() == 0 && bc.count("failed") == 1, "Retirement later succeeds without a second callback");
        OwnerProbe ownerProbe = retainingNativeRequest(clock);
        until(() -> ownerProbe.driver.active() == 1, "owner native request starts");
        ownerProbe.driver.event(7, 8, "", 0);
        until(() -> ownerProbe.failures.get() == 1 && ownerProbe.driver.cancelCalls > 0, "owner failure delivered");
        until(() -> { System.gc(); return ownerProbe.owner.get() == null; },
                "application owner released while native retirement is still blocked");
        check(ownerProbe.driver.active() == 1, "Owner collection does not prematurely free native storage");
        ownerProbe.driver.cancelFailures = 0; finish(ownerProbe.worker);

        Driver changed = new Driver(); Callbacks cc = new Callbacks(); AtomicLong generation = new AtomicLong(1);
        WindowsNsd.Scope generations = new WindowsNsd.Scope() {
            public int[] interfaces() { return new int[] {7}; }
            public Map<Integer, Long> snapshot() { return Map.of(7, generation.get()); }
        };
        WindowsNsd.Browser gb = new WindowsNsd.Browser(changed, "_gesher._tcp", null, generations, cc, clock::get);
        Thread gt = thread(gb); until(() -> cc.count("started") == 1, "generation start");
        changed.event(7, 0, service, 20);
        until(() -> cc.count("found") == 1, "generation initial observation");
        generation.incrementAndGet(); clock.addAndGet(1001);
        until(() -> cc.count("lost") == 1 && changed.releases == 1, "loss and same-index return retires old handle");
        clock.addAndGet(1001);
        until(() -> changed.starts == 2, "returned interface gets fresh native handle");
        changed.event(7, 0, service, 20);
        until(() -> cc.count("found") == 2, "returned interface produces a new observation");
        gb.stop(); finish(gt);
        Driver lostStop = new Driver(); lostStop.cancelFailures = 1; Callbacks lostStopCallback = new Callbacks();
        final int[][] lostScope = {new int[] {7}};
        WindowsNsd.Browser lostStopBrowser = new WindowsNsd.Browser(lostStop, "_gesher._tcp", null,
                () -> lostScope[0], lostStopCallback, clock::get);
        Thread lostStopThread = thread(lostStopBrowser);
        until(() -> lostStopCallback.count("started") == 1, "interface retirement failure start");
        lostScope[0] = new int[0]; clock.addAndGet(1001);
        until(() -> lostStopCallback.count("failed 0") == 1, "interface retirement failure is explicit");
        finish(lostStopThread);
        check(lostStop.active() == 0 && lostStopCallback.count("failed") == 1,
                "Failed interface retirement cleans up without empty successful discovery");

        Driver metadata = new Driver(); Callbacks mc = new Callbacks(); AtomicLong metadataClock = new AtomicLong(0);
        WindowsNsd.Browser mb = new WindowsNsd.Browser(metadata, "_gesher._tcp", null,
                metadata::interfaces, mc, metadataClock::get, () -> 3000000L);
        Thread mt = thread(mb); until(() -> mc.count("started") == 1, "metadata start");
        metadata.event(7, 0, service, 3, 1500, 9, false);
        until(() -> mc.count("found") == 1, "metadata observation");
        check(mc.expiration == 3007500L, "Expiration derives from observed SRV after native queue age");
        metadataClock.set(2000);
        until(metadata::settled, "metadata settled");
        check(mc.count("lost") == 0, "Known SRV lifetime is independent of shorter PTR TTL");
        metadata.event(7, 0, service, 50, 0, -1, false);
        until(metadata::settled, "PTR-only refresh settled");
        metadataClock.set(7500);
        until(() -> mc.count("lost") == 1, "PTR-only refresh does not extend unseen SRV lifetime");
        metadata.event(7, 0, service, 3, 0, -1, false);
        until(() -> mc.count("found") == 2, "PTR-only new observation");
        check(mc.expiration == -1, "Unobserved SRV expiration remains unknown");
        metadata.service(7, service, 5, 0);
        until(metadata::settled, "separate SRV refresh joined to known PTR");
        metadataClock.set(10500);
        Thread.sleep(30);
        check(mc.count("lost") == 1 && mc.count("found") == 2, "Separate SRV refresh preserves live discovery without duplicate found");
        metadata.service(7, service, 0, 0);
        until(() -> mc.count("lost") == 2, "separate SRV goodbye removes prior snapshot");
        check(mc.lostExpiration == 3005000L, "Separate SRV metadata survives in lost snapshot");
        metadata.service(7, service, 7, 1000);
        until(metadata::settled, "SRV before PTR retained as metadata");
        check(mc.count("found") == 2, "SRV alone cannot manufacture a discovery");
        metadata.event(7, 0, service, 3, 0, -1, false);
        until(() -> mc.count("found") == 3, "PTR joins previously observed SRV");
        check(mc.expiration == 3006000L, "Split SRV/PTR delivery retains observed expiration");
        mb.stop(); finish(mt);
        Driver thrown = new Driver(); RuntimeException app = new RuntimeException("application callback");
        try {
            new WindowsNsd.Browser(thrown, "_gesher._tcp", null, thrown::interfaces, new Callbacks() {
                @Override public void started() { throw app; }
            }, clock::get).run();
            throw new AssertionError("Callback error swallowed");
        } catch (RuntimeException error) { check(error == app, "Callback exception propagates unchanged"); }
        until(() -> thrown.active() == 0, "Throwing callback retires native operation");
        for (int n = 0; n < 100; n++) {
            Driver d = new Driver(); Callbacks c = new Callbacks();
            WindowsNsd.Browser b = new WindowsNsd.Browser(d, "_gesher._tcp", null, d::interfaces, c, clock::get);
            b.stop(); b.run();
            check(d.active() == 0 && d.releases == 1 && c.count("stopped") == 1, "Immediate cancellation lifecycle " + n);
        }
        System.out.println("NsdBrowseTransportTest all passed: " + checks);
    }
}
