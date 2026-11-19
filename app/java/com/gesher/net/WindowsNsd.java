/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net;

import android.os.SystemClock;

import java.io.IOException;
import java.util.HashMap;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.function.LongSupplier;
import java.util.function.Consumer;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Internal Windows DNS-SD browse transport. Does not convert TXT data to text. */
public final class WindowsNsd {
    private WindowsNsd() {}

    /** Backend boundary; substitutions are passed explicitly by isolated fixtures. */
    public interface Driver {
        boolean supported();
        int[] interfaces() throws IOException;
        long start(String query, int ifIndex);
        int attachBaseLoss(long handle);
        String[] poll(long handle, int timeoutMillis);
        int cancel(long handle);
        boolean release(long handle);
    }
    public interface Scope {
        int[] interfaces() throws IOException;
        /** A changed generation means that an interface was lost and returned. */
        default Map<Integer, Long> snapshot() throws IOException {
            Map<Integer, Long> result = new HashMap<>();
            for (int index : interfaces()) if (index > 0) result.put(index, 0L);
            return result;
        }
    }
    public interface Callback {
        void started();
        void found(String name, String type, int ifIndex);
        void lost(String name, String type, int ifIndex);
        default void found(String name, String type, int ifIndex, long expirationMillis, String subtype) {
            found(name, type, ifIndex);
        }
        default void lost(String name, String type, int ifIndex, long expirationMillis, String subtype) {
            lost(name, type, ifIndex);
        }
        void stopped();
        void failed(int androidError);
        default void stopFailed(int androidError) { failed(androidError); }
    }
    public interface Session { void stop(); }
    public interface Factory {
        Session start(String type, String subtype, Scope scope, Callback callback);
        int[] interfaces() throws IOException;
    }

    private static final class NativeDriver implements Driver {
        NativeDriver() { System.loadLibrary("gesher_nsd"); }
        public boolean supported() { return nativeSupported(); }
        public int[] interfaces() throws IOException { return nativeInterfaces(); }
        public long start(String query, int index) { return nativeStart(query, index); }
        public int attachBaseLoss(long handle) { return nativeAttachBaseLoss(handle); }
        public String[] poll(long id, int timeout) { return nativePoll(id, timeout); }
        public int cancel(long id) { return nativeCancel(id); }
        public boolean release(long id) { return nativeRelease(id); }
    }
    /** Android enforces INTERNET when the manager connects to its service. */
    public static void enforceInternetPermission() { new NativeDriver().supported(); }

    /** Load only when the application actually starts discovery. */
    public static Factory factory() {
        return new Factory() {
            private Driver driver;
            private synchronized Driver driver() {
                if (driver == null) driver = new NativeDriver();
                return driver;
            }
            public int[] interfaces() throws IOException { return driver().interfaces(); }
            public Session start(String type, String subtype, Scope scope, Callback callback) {
                // Permission denial is synchronous, like the service connection.
                driver().supported();
                Browser session = new Browser(this::driver, type, subtype, scope, callback,
                        SystemClock::elapsedRealtime);
                session.start();
                return session;
            }
        };
    }

    private static native boolean nativeSupported();
    private static native int[] nativeInterfaces() throws IOException;
    private static native long nativeStart(String query, int ifIndex);
    private static native int nativeAttachBaseLoss(long handle);
    private static native String[] nativePoll(long id, int timeoutMillis);
    private static native int nativeCancel(long id);
    private static native boolean nativeRelease(long id);
    private static native int nativeActive();
    private static native long nativeBaseGoodbyes();
    private static native long nativeLastPtrTtl();
    private static native long[] nativeDiagnostics();

    private interface DriverSupplier { Driver get(); }
    // AOSP android-16.0.0_r4 NsdManager label grammar, restricted to one queried
    // subtype. A request with multiple subtypes fails instead of dropping them.
    private static final String LABEL = "_[a-zA-Z0-9_-]{1,61}[a-zA-Z0-9]";
    private static final String SUBTYPE_LABEL = "_[\\x20-\\x2B\\x2D\\x2F-\\x7E]{1,62}";
    private static final Pattern TYPE = Pattern.compile(
            "^(?:(" + SUBTYPE_LABEL + ")\\.)?(" + LABEL + "\\._(?:tcp|udp))\\.?((?:," + SUBTYPE_LABEL + ")*)$");
    private static final Pattern SUBTYPE = Pattern.compile(SUBTYPE_LABEL);
    private static final int CANCELLED = 1223;
    private static final int MAX_INTERFACES = 64;
    private static final int MAX_SERVICES = 4096;

    private static final class Seen {
        final String name;
        long expiry;
        long expirationMillis;
        Seen(String name, long expiry, long expirationMillis) {
            this.name = name; this.expiry = expiry; this.expirationMillis = expirationMillis;
        }
    }
    private static final class Source {
        final int index;
        final long handle;
        final long generation;
        final Map<String, Seen> seen = new LinkedHashMap<>();
        final Map<String, Seen> services = new HashMap<>();
        boolean retiring;
        boolean cancellationSent;
        long nextCancelNanos;
        Source(int index, long handle, long generation) {
            this.index = index; this.handle = handle; this.generation = generation;
        }
    }

    /**
     * One worker owns a request's native handles and observation cache. Native
     * successful native stop, not the Java stop flag, releases those handles.
     */
    public static final class Browser implements Session, Runnable {
        private final DriverSupplier supplier;
        private final String requestedType;
        private final String requestedSubtype;
        private Scope scope;
        private Callback callback;
        private RuntimeException callbackError;
        private final LongSupplier clock;
        private final LongSupplier wallClock;
        private final Map<Integer, Source> sources = new HashMap<>();
        private volatile boolean stop;
        private String type;
        private String query;
        private String suffix;
        private String observedSubtype;
        private int failure = -1;

        private Browser(DriverSupplier supplier, String type, String subtype, Scope scope,
                Callback callback, LongSupplier clock) {
            this(supplier, type, subtype, scope, callback, clock, System::currentTimeMillis);
        }
        private Browser(DriverSupplier supplier, String type, String subtype, Scope scope,
                Callback callback, LongSupplier clock, LongSupplier wallClock) {
            this.supplier = supplier;
            this.requestedType = type;
            this.requestedSubtype = subtype;
            this.scope = scope;
            this.callback = callback;
            this.clock = clock;
            this.wallClock = wallClock;
        }
        /** Explicit test seam; the production factory always uses NativeDriver. */
        public Browser(Driver driver, String type, String subtype, Scope scope,
                Callback callback, LongSupplier clock) {
            this(() -> driver, type, subtype, scope, callback, clock);
        }
        public Browser(Driver driver, String type, String subtype, Scope scope,
                Callback callback, LongSupplier clock, LongSupplier wallClock) {
            this(() -> driver, type, subtype, scope, callback, clock, wallClock);
        }
        public void start() {
            Thread worker = new Thread(this, "Gesher-NsdBrowse");
            worker.setDaemon(true);
            worker.start();
        }
        @Override public void stop() { stop = true; }

        private boolean parseType() {
            Matcher m = TYPE.matcher(requestedType);
            if (!m.matches()) return false;
            type = m.group(2);
            String legacy = m.group(1);
            String comma = m.group(3);
            if (!comma.isEmpty() && legacy == null) {
                if (comma.indexOf(',', 1) >= 0) return false;
                legacy = comma.substring(1);
            }
            String subtype = requestedSubtype != null ? requestedSubtype : legacy;
            if (subtype != null && !SUBTYPE.matcher(subtype).matches()) { failure = 6; return false; }
            observedSubtype = subtype;
            query = (subtype == null ? "" : subtype + "._sub.") + type + ".local";
            suffix = "." + type.toLowerCase(Locale.ROOT) + ".local";
            return true;
        }

        @Override public void run() {
            Driver driver = null;
            try {
                if (!parseType()) {
                    if (failure < 0) failure = 0;
                } else {
                    driver = supplier.get();
                    if (!driver.supported()) failure = 0;
                    else {
                        reconcile(driver);
                        if (failure < 0) invokeCallback(Callback::started);
                        long nextScan = clock.getAsLong() + 1000;
                        while (failure < 0 && !stop) {
                            drain(driver, true);
                            if (failure >= 0 || stop) break;
                            if (clock.getAsLong() >= nextScan) {
                                reconcile(driver);
                                nextScan = clock.getAsLong() + 1000;
                            }
                            expire();
                            if (sources.isEmpty()) pause();
                        }
                    }
                }
            } catch (IOException | LinkageError error) {
                failure = 0;
            } catch (RuntimeException error) {
                // Backend exceptions terminate discovery. Callback exceptions
                // are marked at their boundary and propagate after native-only
                // cleanup is handed to a retirement worker.
                if (callbackError != error) failure = 0;
            } finally {
                stop = true;
                scope = null;
                // A discovery failure is terminal even if Windows cannot yet
                // stop a native request. Detach the application before retries.
                if (failure >= 0 && callback != null) {
                    try { notifyFailure(); }
                    catch (RuntimeException error) { callbackError = error; }
                }
                if (driver != null) {
                    for (Source source : sources.values()) {
                        source.retiring = true;
                        source.seen.clear();
                        source.services.clear();
                    }
                    if (callbackError != null) {
                        callback = null;
                        retireAsynchronously(driver);
                    } else {
                        try { retire(driver); }
                        catch (RuntimeException error) {
                            callback = null;
                            callbackError = error;
                            retireAsynchronously(driver);
                        }
                    }
                }
            }
            RuntimeException error = callbackError;
            callbackError = null;
            if (error != null) throw error;
            Callback last = callback;
            callback = null;
            if (last != null) {
                if (failure >= 0) last.failed(failure);
                else last.stopped();
            }
        }

        private void invokeCallback(Consumer<Callback> action) {
            try { action.accept(callback); }
            catch (RuntimeException error) { callbackError = error; throw error; }
        }

        private void notifyFailure() {
            Callback terminal = callback;
            callback = null;
            // This local's frame returns before retirement. ART's interpreter
            // can retain an out-of-scope local while run() is waiting for Stop.
            terminal.failed(failure);
        }

        private void retire(Driver driver) {
            while (!sources.isEmpty()) {
                drain(driver, false);
                if (!sources.isEmpty()) pause();
            }
        }

        private void retireAsynchronously(Driver driver) {
            if (sources.isEmpty()) return;
            // The callback and Scope are detached. An application exception can
            // propagate without being retained by a permanently failing Stop.
            Thread worker = new Thread(() -> retire(driver), "Gesher-NsdRetire");
            worker.setDaemon(true);
            worker.start();
        }

        private void reconcile(Driver driver) throws IOException {
            Map<Integer, Long> desired = scope.snapshot();
            if (desired.size() > MAX_INTERFACES) { failure = 4; return; }
            for (Source source : sources.values()) {
                Long generation = desired.get(source.index);
                if ((generation == null || generation != source.generation) && !source.retiring) {
                    source.retiring = true;
                    for (Seen seen : source.seen.values()) invokeCallback(c -> c.lost(
                            seen.name, type + ".", source.index, seen.expirationMillis, observedSubtype));
                    source.seen.clear();
                    source.services.clear();
                }
            }
            for (int index : desired.keySet()) {
                if (sources.containsKey(index)) continue;
                long handle = driver.start(query, index);
                if (handle <= 0) { failure = handle == -8 || handle == -56 ? 4 : 0; return; }
                sources.put(index, new Source(index, handle, desired.get(index)));
                // Keep the first accepted query tracked so partial start can
                // retire it even if the independent base query fails.
                if (observedSubtype != null) {
                    int status = driver.attachBaseLoss(handle);
                    if (status != 0) { failure = status == 8 || status == 56 ? 4 : 0; return; }
                }
            }
        }

        private void drain(Driver driver, boolean observations) {
            Iterator<Source> it = sources.values().iterator();
            while (it.hasNext()) {
                Source source = it.next();
                if ((stop || source.retiring) && !source.cancellationSent &&
                        System.nanoTime() >= source.nextCancelNanos) {
                    // On a cancellation failure retain the request storage and
                    // retry. Never report a successful stop while Windows owns it.
                    int error = driver.cancel(source.handle);
                    source.cancellationSent = error == 0;
                    source.nextCancelNanos = System.nanoTime() + 1_000_000_000L;
                    if (error != 0 && !stop && failure < 0) {
                        // Failed interface retirement must not leave a successful
                        // but permanently blocked browse on its next generation.
                        failure = 0;
                    }
                    if (error != 0 && stop && failure < 0 && callback != null) {
                        // Android retires the listener after STOP_DISCOVERY_FAILED.
                        // Cleanup retains only native request storage, not its
                        // owner, and never sends a second terminal callback.
                        Callback terminal = callback;
                        callback = null;
                        terminal.stopFailed(0);
                    }
                }
                String[] events = driver.poll(source.handle, 20);
                if (events == null) continue;
                for (int i = 0; i + 6 < events.length; i += 7) {
                    int status = Integer.parseInt(events[i]);
                    if (status == CANCELLED) {
                        if (!driver.release(source.handle)) throw new IllegalStateException("DNS cancellation not terminal");
                        it.remove();
                        break;
                    }
                    if (status != 0) { if (failure < 0) failure = status == 8 ? 4 : 0; continue; }
                    if (observations && failure < 0 && !stop && !source.retiring) {
                        long ttl = Long.parseLong(events[i + 2]);
                        long queuedMillis = Long.parseLong(events[i + 3]);
                        if ("33".equals(events[i + 6])) {
                            serviceRecord(source, events[i + 1], ttl, queuedMillis);
                        } else {
                            observe(source, events[i + 1], ttl, queuedMillis,
                                    Long.parseLong(events[i + 4]), "1".equals(events[i + 5]));
                        }
                    }
                }
            }
        }

        private String instanceName(String fullName) {
            String canonical = fullName.endsWith(".") ? fullName.substring(0, fullName.length() - 1) : fullName;
            if (!asciiLower(canonical).endsWith(suffix)) return null;
            String name = canonical.substring(0, canonical.length() - suffix.length());
            return name.isEmpty() ? null : name;
        }

        private void removeSeen(Source source, String key) {
            Seen old = source.seen.remove(key);
            source.services.remove(key);
            if (old != null) invokeCallback(c -> c.lost(old.name, type + ".", source.index,
                    old.expirationMillis, observedSubtype));
        }

        private void serviceRecord(Source source, String fullName, long ttl, long queuedMillis) {
            String name = instanceName(fullName);
            if (name == null) return;
            String key = asciiLower(name);
            if (ttl == 0) { removeSeen(source, key); return; }
            long lifetime = Math.max(0, ttl * 1000L - queuedMillis);
            if (lifetime == 0) return;
            if (!source.services.containsKey(key) && source.services.size() >= MAX_SERVICES) {
                failure = 4; return;
            }
            Seen record = new Seen(name, clock.getAsLong() + lifetime, wallClock.getAsLong() + lifetime);
            source.services.put(key, record);
            Seen old = source.seen.get(key);
            if (old != null) {
                old.expiry = record.expiry;
                old.expirationMillis = record.expirationMillis;
            }
            // An SRV is metadata, never proof of the requested PTR membership.
        }

        private void observe(Source source, String fullName, long ttl, long queuedMillis,
                long serviceTtl, boolean goodbye) {
            String name = instanceName(fullName);
            if (name == null) return;
            String key = asciiLower(name);
            // API36 forwards the previously cached snapshot immediately on a
            // base PTR/SRV goodbye. Do not invent a new expiration for that event.
            if (goodbye) {
                removeSeen(source, key);
                return;
            }
            // API36 does not interpret a subtype-only PTR goodbye as the entire
            // service going offline. Keep its last positive observation.
            if (ttl == 0) return;
            if (serviceTtl >= 0) serviceRecord(source, fullName, serviceTtl, queuedMillis);
            if (failure >= 0) return;
            Seen old = source.seen.get(key);
            Seen service = source.services.get(key);
            if (service != null && service.expiry <= clock.getAsLong()) {
                source.services.remove(key);
                service = null;
            }
            long lifetime = Math.max(0, (serviceTtl >= 0 ? serviceTtl : ttl) * 1000L - queuedMillis);
            long expiry = service == null ? clock.getAsLong() + lifetime : service.expiry;
            // AOSP's expiration metadata derives from SRV, not PTR TTL.
            // A PTR-only Windows result has unknown expiration; the manager keeps
            // it null instead of claiming a finite lifetime for an unseen SRV.
            long expirationMillis = service == null ? -1 : service.expirationMillis;
            if (old == null && lifetime == 0) return;
            if (old == null) {
                if (source.seen.size() >= MAX_SERVICES) { failure = 4; return; }
                source.seen.put(key, new Seen(name, expiry, expirationMillis));
                invokeCallback(c -> c.found(name, type + ".", source.index, expirationMillis, observedSubtype));
            } else {
                if (service != null || old.expirationMillis < 0) old.expiry = expiry;
                if (service != null) old.expirationMillis = expirationMillis;
            }
        }

        private static String asciiLower(String text) {
            StringBuilder folded = new StringBuilder(text.length());
            for (int i = 0; i < text.length(); i++) {
                char c = text.charAt(i);
                folded.append(c >= 'A' && c <= 'Z' ? (char) (c + ('a' - 'A')) : c);
            }
            return folded.toString();
        }

        private void expire() {
            long now = clock.getAsLong();
            for (Source source : sources.values()) {
                source.services.values().removeIf(record -> record.expiry <= now);
                Iterator<Seen> it = source.seen.values().iterator();
                while (it.hasNext()) {
                    Seen seen = it.next();
                    if (seen.expiry <= now) {
                        it.remove();
                        invokeCallback(c -> c.lost(seen.name, type + ".", source.index,
                                seen.expirationMillis, observedSubtype));
                    }
                }
            }
        }
        private static void pause() {
            try { Thread.sleep(20); } catch (InterruptedException ignored) { /* stop is explicit */ }
        }
    }
}
