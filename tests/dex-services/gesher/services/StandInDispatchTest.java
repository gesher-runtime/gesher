/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.pm.ParceledListSlice;
import android.os.IBinder;
import android.os.IInterface;
import com.gesher.server.StandIn;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.PrintStream;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

/** Actual Proxy dispatch; the host runner substitutes only Binder and list-slice boundaries. */
public final class StandInDispatchTest {
    private static int checks;
    public static final IOException FAILURE = new IOException("original failure");

    public interface Sample extends IInterface {
        int ping();
        String overload(int value);
        String overload(String value);
        int inherited();
        int missing();
        List<String> list();
        ParceledListSlice<?> slice();
        void checked() throws IOException;
        default String interfaceDefault() { return "interface-default"; }

        class Default implements Sample {
            public IBinder asBinder() { return null; }
            public int ping() { return 0; }
            public String overload(int value) { return null; }
            public String overload(String value) { return null; }
            public int inherited() { return 0; }
            public int missing() { return 7; }
            public List<String> list() { return null; }
            public ParceledListSlice<?> slice() { return null; }
            public void checked() throws IOException {}
        }
        class Stub { public static final String DESCRIPTOR = "gesher.tests.StandInSample"; }
    }

    public static class Parent extends Sample.Default {
        public int inherited() { return 23; }
    }
    public static class Impl extends Parent {
        public int ping() { return 17; }
        public String overload(int value) { return "int:" + value; }
        public String overload(String value) { return "string:" + value; }
        public void checked() throws IOException { throw FAILURE; }
    }
    public static class Other extends Impl {
        public int ping() { return 29; }
    }

    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }

    public static void main(String[] args) throws Exception {
        boolean host = args.length == 1 && "--host".equals(args[0]);
        if (args.length != 0 && !host) throw new IllegalArgumentException("Expected --host or no arguments");
        if (!host) System.loadLibrary("android_runtime");
        StandIn.setStrict(false);
        StandIn<Sample> wrapper = StandIn.of(Sample.class, new Impl());
        Sample proxy = wrapper.asInterface();
        Sample strictInstance = StandIn.ofStrictDefaults(Sample.class, new Impl()).asInterface();
        try { strictInstance.missing(); throw new AssertionError("Per-service strict defaults ignored"); }
        catch (UnsupportedOperationException expected) { checks++; }
        check(strictInstance.ping() == 17 && strictInstance.inherited() == 23,
                "Per-service strict defaults preserve own and inherited implementations");
        StandIn.setStrict(true);
        StandIn.setStrict(false);
        try { strictInstance.missing(); throw new AssertionError("Global mode disabled per-service strict defaults"); }
        catch (UnsupportedOperationException expected) { checks++; }
        check(StandIn.of(Sample.class, new Impl()).asInterface().missing() == 7,
                "Per-service strict defaults do not alter unrelated ordinary wrappers");
        check(proxy.equals(proxy) && !proxy.equals(new Object()), "Proxy equals remains identity-based");
        check(proxy.hashCode() == System.identityHashCode(proxy), "Proxy identity hash");
        check("StandIn{Sample}".equals(proxy.toString()), "Proxy descriptive string");
        check(proxy.asBinder() == wrapper.asBinder() && proxy.asBinder() != null, "Binder fast path");
        check(proxy.ping() == 17 && proxy.ping() == 17, "Implemented method cold and warm");
        check("int:4".equals(proxy.overload(4)), "Primitive overload");
        check("string:x".equals(proxy.overload("x")), "Reference overload");
        check(proxy.inherited() == 23, "Inherited implementation is not an unimplemented default");
        try { proxy.checked(); throw new AssertionError("Expected the original exception"); }
        catch (IOException expected) { check(expected == FAILURE, "Invocation cause identity preserved"); }

        PrintStream saved = System.err;
        ByteArrayOutputStream log = new ByteArrayOutputStream();
        try (PrintStream output = new PrintStream(log, true, "UTF-8")) {
            System.setErr(output);
            StandIn.setTag("first-tag");
            check(proxy.missing() == 7 && proxy.missing() == 7, "Default value remains callable");
            StandIn.setStrict(true);
            try { proxy.missing(); throw new AssertionError("Strict mode ignored after cache warm-up"); }
            catch (UnsupportedOperationException expected) { checks++; }
            check(proxy.ping() == 17, "Strict mode still permits implemented methods");
            StandIn.setStrict(false);
            check(proxy.missing() == 7, "Strict mode can be disabled after warm-up");
            StandIn.setTag("second-tag");
            List<String> first = proxy.list();
            first.add("mutable");
            List<String> second = proxy.list();
            check(first != second && second.isEmpty(), "Each default list is fresh and mutable");
            check(proxy.slice() != proxy.slice() && proxy.slice().getList().isEmpty(),
                    "Each default list slice is fresh and empty");
            check("interface-default".equals(proxy.interfaceDefault()), "Interface default dispatch");
        } finally {
            System.setErr(saved);
            StandIn.setStrict(false);
            StandIn.setTag("gesher-app");
        }
        String messages = log.toString("UTF-8");
        check(messages.indexOf("Sample.missing") == messages.lastIndexOf("Sample.missing"),
                "Default logging remains once per method name");
        check(messages.contains("first-tag: stand-in Sample.missing")
                        && messages.contains("second-tag: stand-in Sample.list"), "Log tag remains dynamic");
        check(StandIn.of(Sample.class, new Other()).asInterface().ping() == 29 && proxy.ping() == 17,
                "Dispatch caches do not cross implementation instances");

        Sample concurrent = StandIn.of(Sample.class, new Impl()).asInterface();
        CountDownLatch start = new CountDownLatch(1);
        AtomicReference<Throwable> failed = new AtomicReference<>();
        Thread[] workers = new Thread[8];
        for (int n = 0; n < workers.length; n++) {
            workers[n] = new Thread(() -> {
                try {
                    start.await();
                    for (int i = 0; i < 2000; i++) {
                        if (concurrent.ping() != 17 || concurrent.inherited() != 23
                                || !"int:4".equals(concurrent.overload(4))) {
                            throw new AssertionError("Concurrent dispatch mismatch");
                        }
                    }
                } catch (Throwable failure) { failed.compareAndSet(null, failure); }
            });
            workers[n].start();
        }
        start.countDown();
        for (Thread worker : workers) {
            worker.join(10000);
            check(!worker.isAlive(), "Concurrent invocation completes within its deadline");
        }
        check(failed.get() == null, "Concurrent first invocation and steady dispatch remain correct");
        System.out.println("StandInDispatchTest all passed: " + checks + " checks");
    }
}
