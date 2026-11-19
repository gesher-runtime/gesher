/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.Network;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import com.gesher.net.WindowsNetworks;
import com.gesher.net.WindowsNsdResolver;
import com.gesher.net.WindowsNsdResolver.Interface;

import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;

/** Required real Windows raw-socket/mDNS gate; no injected transport or network mapping. */
public final class NsdResolveWindowsTest {
    private static int checks, command;
    private static final AtomicReference<String> failure = new AtomicReference<>();
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private static void await(BooleanSupplier condition, String message) throws Exception {
        long deadline = System.nanoTime() + 15_000_000_000L;
        while (!condition.getAsBoolean() && failure.get() == null) {
            if (System.nanoTime() >= deadline) throw new AssertionError("Fixture deadline: " + message);
            Thread.sleep(10);
        }
        check(failure.get() == null, message + ": " + failure.get());
    }
    private static int[] active() {
        try {
            Method method = WindowsNsdResolver.class.getDeclaredMethod("nativeActive"); method.setAccessible(true);
            return (int[]) method.invoke(null);
        } catch (Exception error) { throw new AssertionError(error); }
    }
    private static void retired() throws Exception {
        await(() -> Arrays.equals(active(), new int[] {0, 0, 0, 0, 0, 0}), "all native requests, sockets, watchers, entered callbacks and progress owners retired");
    }
    private static void peer(Path control, String mode) throws Exception {
        String serial = Integer.toString(++command);
        Path tmp = control.resolve("command-new"); Files.writeString(tmp, serial + " " + mode, StandardCharsets.UTF_8);
        Files.move(tmp, control.resolve("command"), StandardCopyOption.REPLACE_EXISTING);
        await(() -> { try { return Files.readString(control.resolve("ack")).trim().equals(serial); } catch (Exception pending) { return false; } }, "peer " + mode);
    }
    private static final class Listener implements NsdManager.ResolveListener {
        final AtomicInteger resolved = new AtomicInteger(), stopped = new AtomicInteger();
        volatile NsdServiceInfo value;
        public void onServiceResolved(NsdServiceInfo info) { value = info; resolved.incrementAndGet(); }
        public void onResolutionStopped(NsdServiceInfo info) { value = info; stopped.incrementAndGet(); }
        public void onResolveFailed(NsdServiceInfo info, int error) { failure.set("resolution failed " + error); }
        public void onStopResolutionFailed(NsdServiceInfo info, int error) { failure.set("retirement failed " + error); }
    }
    private static NsdServiceInfo request(String type, String name, Interface scope) {
        NsdServiceInfo value = new NsdServiceInfo(); value.setServiceType(type); value.setServiceName(name);
        if (scope.netId != 0) value.setNetwork(new Network(scope.netId)); else value.setInterfaceIndex(scope.index);
        return value;
    }
    public static void main(String[] args) throws Exception {
        if (args.length != 7) throw new IllegalArgumentException("type instance canonical-index family-index control family address");
        System.loadLibrary("android_runtime"); System.loadLibrary("gesher_nsd_resolve");
        String type = args[0], name = args[1]; int index = Integer.parseInt(args[2]), familyIndex = Integer.parseInt(args[3]);
        Path control = Path.of(args[4]); int family = Integer.parseInt(args[5]);
        if (!type.matches("_gr[0-9a-f]{12}\\._tcp")) throw new IllegalArgumentException("Owned random fixture type required");
        retired();
        Class<?> nativeType = Class.forName("com.gesher.net.WindowsNsdResolver$NativeDriver");
        Constructor<?> constructor = nativeType.getDeclaredConstructor(); constructor.setAccessible(true);
        WindowsNsdResolver.Driver inventory = (WindowsNsdResolver.Driver) constructor.newInstance();
        Interface selected = null;
        try { for (Interface scope : inventory.interfaces()) if (scope.index == index) selected = scope; }
        finally { check(inventory.close(), "actual inventory owner released"); }
        retired();
        check(selected != null && selected.luid != 0, "actual adapter LUID/index present");
        check((family == 4 ? selected.ipv4Index : selected.ipv6Index) == familyIndex, "separate actual family index");
        Interface scope = selected;
        List<String> ready = Files.readAllLines(control.resolve("ready"));
        InetAddress address = InetAddress.getByName(args[6].split("%", 2)[0]);
        NsdManager manager = new NsdManager(null); Listener listener = new Listener();
        NsdServiceInfo input = request(type, name, scope);
        manager.resolveService(input, Runnable::run, listener);
        await(() -> listener.resolved.get() == 1, "independent actual IPv" + family + " peer resolved");
        retired();
        NsdServiceInfo result = listener.value;
        NsdResolvePeerTest.verify(result, type, name, ready.get(2), Integer.parseInt(ready.get(1)), address);
        check(result.getExpirationTime() != null && result.getExpirationTime().toEpochMilli() > System.currentTimeMillis() + 90000,
                "actual SRV expiration metadata");
        if (scope.netId == 0) check(result.getNetwork() == null && result.getInterfaceIndex() == scope.index, "local-only scope remains unmapped");
        else check(result.getNetwork().equals(new Network(scope.netId)) && result.getInterfaceIndex() == 0, "existing ConnectivityManager Network mapping");
        if (family == 6 && address.isLinkLocalAddress()) {
            check(((Inet6Address) result.getHostAddresses().get(0)).getScopeId() == scope.ipv6Index, "actual IPv6 scope");
        }
        try { manager.stopServiceResolution(listener); throw new AssertionError("completed listener still active"); }
        catch (IllegalArgumentException expected) { check(true, "terminal listener removed"); }
        peer(control, "silent");
        manager.resolveService(input, Runnable::run, listener);
        await(() -> {
            int[] state = active();
            return state[0] == 1 && state[1] > 0 && state[2] == 2 && state[4] == 1 && state[5] == 1;
        }, "native cancellation target owns actual sockets, both watchers, a completed poll and a sent query");
        System.out.println("NSD_RESOLVE_WINDOWS_CANCEL_READY " + Arrays.toString(active()));
        manager.stopServiceResolution(listener);
        await(() -> listener.stopped.get() == 1, "successful cancellation waits for native closure");
        retired(); check(listener.value == input, "stop returns original request");
        peer(control, "answer");
        manager.resolveService(input, Runnable::run, listener);
        await(() -> listener.resolved.get() == 2, "listener reused for a fresh real query");
        retired(); check(listener.stopped.get() == 1, "no late stop callback after reuse");
        Listener absent = new Listener(); NsdServiceInfo unavailable = request(type, name, scope);
        unavailable.setNetwork(new Network(Integer.MAX_VALUE));
        manager.resolveService(unavailable, Runnable::run, absent);
        await(() -> {
            int[] state = active();
            return state[0] == 1 && state[2] == 2 && state[4] == 1;
        }, "absent Network completes a real scope reconciliation and poll with both watchers registered");
        int[] absentState = active();
        check(absentState[1] == 0 && absentState[5] == 0, "absent Network opens no fallback sockets or sends");
        System.out.println("NSD_RESOLVE_WINDOWS_ABSENT_READY " + Arrays.toString(absentState));
        manager.stopServiceResolution(absent); await(() -> absent.stopped.get() == 1, "absent Network canceled"); retired();
        try {
            WindowsNetworks.setInternetAllowed(false);
            try { new NsdManager(null); throw new AssertionError("INTERNET native guard bypassed"); }
            catch (SecurityException expected) { check(true, "native permission guard denies acquisition"); }
        } finally { WindowsNetworks.setInternetAllowed(true); }
        peer(control, "quit");
        System.out.println("NsdResolveWindowsTest all passed: " + checks + "; binaryAssertions=" + NsdResolvePeerTest.verificationCount()
                + "; family=" + family + "; production=Winsock-raw-mDNS; nativeActive=0; ordinaryApkIdentity=false");
    }
}
