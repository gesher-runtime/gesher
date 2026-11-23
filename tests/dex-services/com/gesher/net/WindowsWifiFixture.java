/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net;

import android.net.DhcpInfo;
import android.net.wifi.ScanResult;
import android.net.wifi.SupplicantState;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** Isolated fixture: native responses and clocks are injected only by this test class. */
public final class WindowsWifiFixture {
    private static int checks;
    private static volatile String answer;
    private static volatile CountDownLatch entered;
    private static volatile CountDownLatch release;
    private static final AtomicInteger calls = new AtomicInteger();
    private static final String IFACE = "if\t6\t1\t1\t";
    private static final String END = "\taa:bb:cc:11:22:33\t5180\t-58\t866000\t720000\t10\t7\t1\n";
    private static final String BSS = "bss\t4e6574\t10:20:30:40:50:60\t5180\t-55\t8000000\t10\t17\t30140100000fac040100000fac040100000fac020000\n";
    private static native void installNativeBoundary();

    private static void check(boolean yes, String reason) {
        if (!yes) throw new AssertionError(reason);
        checks++;
    }
    private static String hex(byte[] bytes) {
        StringBuilder result = new StringBuilder();
        for (byte value : bytes) result.append(String.format("%02x", value & 255));
        return result.toString();
    }
    private static Object get(String name) throws Exception {
        Field field = WindowsWifi.class.getDeclaredField(name); field.setAccessible(true);
        return field.get(null);
    }
    private static void set(String name, Object value) throws Exception {
        Field field = WindowsWifi.class.getDeclaredField(name); field.setAccessible(true); field.set(null, value);
    }
    private static void publish(WindowsWifi.Data data) throws Exception {
        set("sData", data); set("sReadNanos", System.nanoTime());
    }
    private static void idle() throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (get("sOperation") != null && System.nanoTime() < deadline) Thread.sleep(5);
        check(get("sOperation") == null, "Native worker did not release ownership");
    }
    public static String query(boolean scan, java.util.function.BooleanSupplier eligible) {
        calls.incrementAndGet();
        CountDownLatch wait = release;
        CountDownLatch started = entered;
        if (started != null) started.countDown();
        if (wait != null) {
            try { if (!wait.await(5, TimeUnit.SECONDS)) throw new AssertionError("Fixture not released"); }
            catch (InterruptedException e) { Thread.currentThread().interrupt(); throw new AssertionError(e); }
        }
        return scan && !eligible.getAsBoolean() ? null : answer;
    }
    public static void parser() throws Exception {
        String ssid = "\u200fספרים\u200e\t\n";
        WindowsWifi.Data data = WindowsWifi.parse("ok\t1\n" + IFACE + hex(ssid.getBytes(StandardCharsets.UTF_8)) + END + BSS, 10_000_000L);
        check(data != null && data.states.size() == 1 && data.scans.size() == 1, "Native wire data parsed");
        check(ssid.equals(new String(data.states.get(6).ssid, StandardCharsets.UTF_8)), "Raw SSID bytes retain bidi, Unicode, tabs and newlines");
        check(data.scans.get(0).timestamp == 8_000_000L, "Cached BSS observation retains its native elapsed timestamp");
        check("[RSN-PSK-CCMP][ESS]".equals(data.scans.get(0).capabilities), "Actual RSN suites decoded");
        check(data.scans.get(0).getSecurityTypes()[0] == WifiInfo.SECURITY_TYPE_PSK, "SDK classifies real PSK correctly");
        check(WindowsWifi.parse("Access is denied.", 10_000_000) == null, "Errors are not successful empty scans");
        check(WindowsWifi.parse("ok\t1\n" + IFACE + "0z" + END, 10_000_000) == null, "Malformed hex rejected");
        check(WindowsWifi.parse("ok\t1\n" + IFACE + "00" + END + IFACE + "00" + END, 10_000_000) == null, "Duplicate interface identity rejected");
        check(WindowsWifi.parse("ok\t1\n" + BSS.replace("8000000", "-1"), 10_000_000).scans.isEmpty(), "Negative elapsed timestamp rejected");
        check(WindowsWifi.parse("ok\t1\n" + BSS, 1000).scans.isEmpty(), "Future scan observation discarded");
        check(WindowsWifi.parse("ok\t0\n" + IFACE + "00" + END, 10_000_000).scansValid == false, "Unavailable scans differ from empty successful list");
        check(WindowsWifi.securityType(8) == WifiInfo.SECURITY_TYPE_EAP_WPA3_ENTERPRISE_192_BIT, "WPA3 Enterprise 192 is not SAE");
        check(WindowsWifi.securityType(11) == WifiInfo.SECURITY_TYPE_EAP_WPA3_ENTERPRISE, "WPA3 Enterprise mapped");
        check(WindowsWifi.securityType(9) == WifiInfo.SECURITY_TYPE_SAE, "SAE mapped");
        check(WindowsWifi.securityType(10) == WifiInfo.SECURITY_TYPE_OWE, "OWE mapped");
        check(WindowsWifi.securityType(500) == WifiInfo.SECURITY_TYPE_UNKNOWN, "Unknown security preserved");
        check(WindowsWifi.standard(3) == ScanResult.WIFI_STANDARD_UNKNOWN, "Unsupported PHY is unknown");
        check(WindowsWifi.standard(11) == ScanResult.WIFI_STANDARD_11BE, "EHT maps to 11be");
        check(WindowsWifi.capabilities(new byte[0], 1).equals("[ESS]"), "Open infrastructure bits preserved");
        check(WindowsWifi.capabilities(new byte[0], 18).equals("[WEP][IBSS]"), "Privacy-only IBSS is not open infrastructure");
        check(WindowsWifi.capabilities(new byte[] {48, 4, 1, 0}, 17).contains("?"), "Truncated RSN never classified open");
        check(WindowsWifi.capabilities(hexBytes("30140100000fac040100000fac040100000fac08c000"), 17)
                .equals("[RSN-SAE-CCMP][MFPR][MFPC][ESS]"), "SAE with actual PMF bits");
        check(WindowsWifi.capabilities(hexBytes("30140100000fac040100000fac040100000fac05c000"), 17)
                .contains("EAP/SHA256"), "WPA3 Enterprise RSN is EAP, not SAE");
        check(WindowsWifi.capabilities(hexBytes("30140100000fac040100000fac040100000facff0000"), 17)
                .contains("?"), "Unknown AKM remains unknown");
        ScanResult protectedUnknown = new ScanResult();
        protectedUnknown.capabilities = WindowsWifi.capabilities(new byte[] {48},17);
        check(protectedUnknown.getSecurityTypes().length == 0, "Malformed protected security never reported as OPEN or WEP");
        protectedUnknown.capabilities = WindowsWifi.capabilities(new byte[] {68,0},17);
        check(protectedUnknown.getSecurityTypes().length == 0, "Unimplemented WAPI is unknown, not WEP");
    }
    private static byte[] hexBytes(String value) {
        byte[] bytes = new byte[value.length() / 2];
        for (int i = 0; i < bytes.length; i++) bytes[i] = (byte)Integer.parseInt(value.substring(i*2, i*2+2),16);
        return bytes;
    }
    public static void host() throws Exception {
        installNativeBoundary();
        set("sLoaded", true);
        publish(WindowsWifi.parse("ok\t1\nradio\t1\n", 10_000_000));
        check(WindowsWifi.wifiState() == WifiManager.WIFI_STATE_ENABLED
                && WindowsWifi.connectionInfo(6).getSupplicantState() == SupplicantState.UNINITIALIZED,
                "Unmapped enabled radio has no invented connection or interface identity");
        answer = "ok\t1\nradio\t1\n" + BSS;
        AtomicInteger unmappedCompletions = new AtomicInteger();
        check(WindowsWifi.startScan(ok -> { if (ok) unmappedCompletions.incrementAndGet(); }),
                "Enabled unmapped radio can accept an actual scan");
        idle();
        long unmappedDeadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
        while (unmappedCompletions.get() != 1 && System.nanoTime() < unmappedDeadline) Thread.sleep(5);
        check(unmappedCompletions.get() == 1 && WindowsWifi.scanResults().size() == 1
                && WindowsWifi.configuredNetworks().isEmpty() && WindowsWifi.connectionInfo().getNetworkId() == -1,
                "Unmapped radio publishes scan observations without inventing saved profiles");
        calls.set(0);
        publish(WindowsWifi.parse("ok\t1\nradio\t-1\n", 10_000_000));
        check(WindowsWifi.wifiState() == WifiManager.WIFI_STATE_UNKNOWN,
                "Unmapped unreadable radio stays unknown");
        WindowsWifi.Data data = WindowsWifi.parse("ok\t1\n" + IFACE + "4e6574" + END + BSS, 10_000_000);
        data.networks = WindowsNetworks.parse("internet\t1\nnet\t6\twifi\t1500\t1\t1\t0\t0\t0\t1\t1\tWiFi\tNative\n"
                + "addr\t6\t10.0.0.4\t24\ngw\t6\t10.0.0.1\ndns\t6\t1.1.1.1\n");
        publish(data);
        check(WindowsWifi.wifiState() == WifiManager.WIFI_STATE_ENABLED, "Known native radio enabled");
        WifiInfo info = WindowsWifi.connectionInfo();
        check("\"Net\"".equals(info.getSSID()) && info.getSupplicantState() == SupplicantState.COMPLETED, "SSID and real connection populated");
        check(info.getNetworkId() == -1, "No invented Android profile ID");
        check(info.getIpAddress() == 0x0400000a && info.getLinkSpeed() == 720, "Matching adapter IPv4 and rate");
        check("\"Net\"".equals(WindowsWifi.connectionInfo(6).getSSID())
                && WindowsWifi.connectionInfo(6).getIpAddress() == 0x0400000a, "Service exact-index lookup keeps matching adapter data");
        check(WindowsWifi.connectionInfo(7).getSupplicantState() == SupplicantState.UNINITIALIZED,
                "Service unknown-index lookup never substitutes the default adapter");
        WindowsNetworks.Net other = new WindowsNetworks.Net(); other.ifIndex=7; other.kind="wifi";
        check(WindowsWifi.connectionInfo(other).getSupplicantState() == SupplicantState.UNINITIALIZED, "Unknown adapter never gets another SSID");
        WindowsNetworks.Net cleared = new WindowsNetworks.Net(); cleared.ifIndex=6; cleared.kind="wifi";
        check(WindowsWifi.connectionInfo(cleared).getIpAddress() == 0, "Fresh overlay without IP clears old address");
        DhcpInfo dhcp = WindowsWifi.dhcpInfo();
        check(dhcp.ipAddress == 0x0400000a && dhcp.netmask == 0x00ffffff && dhcp.gateway == 0x0100000a, "Known IPv4 configuration mapped");
        check(dhcp.serverAddress == 0 && dhcp.leaseDuration == 0, "Gateway and assumed lease are not DHCP evidence");
        check(WindowsWifi.configuredNetworks().isEmpty(), "No invented saved Windows profile");
        List<ScanResult> scans = WindowsWifi.scanResults(); scans.get(0).SSID = "changed"; scans.get(0).BSSID = "changed";
        check("Net".equals(WindowsWifi.scanResults().get(0).SSID) && !WindowsWifi.scanResults().get(0).BSSID.equals("changed"), "Returned scan objects cannot mutate cache");
        answer = "ok\t1\n" + IFACE + "4e6577" + END + BSS;
        entered = new CountDownLatch(1); release = new CountDownLatch(1);
        AtomicInteger completions = new AtomicInteger(); AtomicInteger successes = new AtomicInteger();
        check(WindowsWifi.startScan(ok -> { completions.incrementAndGet(); if (ok) successes.incrementAndGet(); }), "Scan accepted");
        check(entered.await(2,TimeUnit.SECONDS), "Native scan running");
        check(WindowsWifi.startScan(ok -> { throw new IllegalStateException("fixture callback"); }), "Exceptional callback coalesced");
        check(WindowsWifi.startScan(ok -> { completions.incrementAndGet(); if (ok) successes.incrementAndGet(); }), "Second requester retained");
        long started = System.nanoTime();
        for (int i=0;i<100;i++) { WindowsWifi.connectionInfo(); WindowsWifi.scanResults(); WindowsWifi.wifiState(); }
        check(System.nanoTime()-started < TimeUnit.SECONDS.toNanos(1), "Read APIs never wait for native scan");
        check(calls.get()==1, "One native request for coalesced callers");
        release.countDown(); idle();
        long completionDeadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(2);
        while(completions.get()!=2 && System.nanoTime()<completionDeadline) Thread.sleep(5);
        check(completions.get()==2 && successes.get()==2, "Every requester notified once despite another callback exception");
        check("\"New\"".equals(WindowsWifi.connectionInfo().getSSID()), "Completed scan publishes real new state");
        // Explicit invalidation while native work is pending must not resurrect old data.
        entered = new CountDownLatch(1); release = new CountDownLatch(1);
        AtomicInteger invalidated = new AtomicInteger();
        check(WindowsWifi.startScan(ok -> { if(!ok) invalidated.incrementAndGet(); }), "Second scan accepted");
        check(entered.await(2, TimeUnit.SECONDS), "Second native query entered");
        WindowsWifi.invalidateCache(); release.countDown(); idle();
        completionDeadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(2);
        while(invalidated.get()!=1 && System.nanoTime()<completionDeadline) Thread.sleep(5);
        check(get("sData")==null && invalidated.get()==1, "Invalidated generation cannot publish or report success");
        // Exercise the exact production deadline callback without a 15 second test sleep.
        publish(data); entered=new CountDownLatch(1); release=new CountDownLatch(1);
        AtomicInteger timeouts=new AtomicInteger();
        check(WindowsWifi.startScan(ok -> {if(!ok)timeouts.incrementAndGet();}), "Timeout candidate accepted");
        check(entered.await(2,TimeUnit.SECONDS), "Timeout candidate entered");
        Object operation=get("sOperation");
        Method expire=WindowsWifi.class.getDeclaredMethod("expire",operation.getClass()); expire.setAccessible(true);
        expire.invoke(null,operation); expire.invoke(null,operation);
        check(timeouts.get()==1, "Deadline callback once");
        check(!WindowsWifi.startScan(ok -> {}), "Timed-out native query retains worker ownership without backlog");
        release.countDown(); idle();
        check(timeouts.get()==1, "Late native completion cannot notify twice");
        // Revocation of Windows location access clears previously cached BSS data.
        publish(data); entered=new CountDownLatch(1); release=new CountDownLatch(1);
        answer="ok\t2\n" + IFACE + "4e6577" + END;
        AtomicInteger windowsDenied=new AtomicInteger();
        check(WindowsWifi.startScan(ok -> {if(!ok)windowsDenied.incrementAndGet();}), "Windows denial candidate accepted");
        check(entered.await(2,TimeUnit.SECONDS), "Windows denial query entered");release.countDown();idle();
        completionDeadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(2);
        while(windowsDenied.get()!=1 && System.nanoTime()<completionDeadline) Thread.sleep(5);
        check(windowsDenied.get()==1 && WindowsWifi.scanResults().isEmpty(), "Windows denial clears stale private scan cache");
        answer="ok\t1\n" + IFACE + "4e6577" + END + BSS;
                // Permission revoked during native setup is checked at the physical-scan boundary.
        publish(data); entered=new CountDownLatch(1); release=new CountDownLatch(1);
        java.util.concurrent.atomic.AtomicBoolean allowed=new java.util.concurrent.atomic.AtomicBoolean(true);
        AtomicInteger revoked=new AtomicInteger();
        check(WindowsWifi.startScan(allowed::get,ok -> {if(!ok)revoked.incrementAndGet();}), "Guarded scan accepted");
        check(entered.await(2,TimeUnit.SECONDS), "Guarded scan entered native setup");
        allowed.set(false); release.countDown(); idle();
        completionDeadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(2);
        while(revoked.get()!=1 && System.nanoTime()<completionDeadline) Thread.sleep(5);
        check(revoked.get()==1, "Revoked requester cannot authorize a queued scan");
        publish(data); entered=new CountDownLatch(1); release=new CountDownLatch(1);
        AtomicInteger validCoalesced=new AtomicInteger();
        check(WindowsWifi.startScan(() -> false,ok -> {if(ok)validCoalesced.incrementAndGet();}), "Initially queued requester accepted");
        check(entered.await(2,TimeUnit.SECONDS), "Coalesced guard setup entered");
        check(WindowsWifi.startScan(() -> true,ok -> {if(ok)validCoalesced.incrementAndGet();}), "Live authorized coalesced requester accepted");
        release.countDown(); idle();
        completionDeadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(2);
        while(validCoalesced.get()!=2 && System.nanoTime()<completionDeadline) Thread.sleep(5);
        check(validCoalesced.get()==2, "One authorized live requester permits shared physical scan");
                // Fresh malformed/error responses clear known state and do not invent empty success.
        publish(data); entered=new CountDownLatch(1); release=new CountDownLatch(1); answer="error";
        AtomicInteger failures=new AtomicInteger();
        check(WindowsWifi.startScan(ok -> {if(!ok)failures.incrementAndGet();}), "Error candidate accepted");
        check(entered.await(2,TimeUnit.SECONDS), "Error query entered"); release.countDown(); idle();
        completionDeadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(2);
        while(failures.get()!=1 && System.nanoTime()<completionDeadline) Thread.sleep(5);
        check(failures.get()==1 && get("sData")==null, "Error produces unavailable state and failed result");
        int before=calls.get(); for(int i=0;i<100;i++)WindowsWifi.wifiState();
        check(calls.get()==before, "Failed native reads respect retry backoff");
        publish(WindowsWifi.parse("ok\t1\n",10_000_000));
        check(WindowsWifi.wifiState()==WifiManager.WIFI_STATE_DISABLED && !WindowsWifi.startScan(ok -> {}), "No adapter does not accept scans");
    }
    private static final class Recorder implements WindowsWifi.Events {
        final List<String> events = new java.util.concurrent.CopyOnWriteArrayList<>();
        @Override public void onWifiStateChanged(int previous, int current) { events.add("state " + previous + ">" + current); }
        @Override public void onScanResultsAvailable() { events.add("scan"); }
        @Override public void onConnectionChanged() { events.add("connection"); }
    }
    private static void await(List<String> events, int size) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
        while (events.size() < size && System.nanoTime() < deadline) Thread.sleep(5);
    }
    @SuppressWarnings("unchecked")
    public static void events() throws Exception {
        WindowsWifi.Hardware hardware = WindowsWifi.parseHardware("hw\t2\ncap\t80\t640\t16\ncap\t1024\t128\t2147483648\n");
        check(hardware != null && hardware.interfaces == 2 && hardware.phy(4) && hardware.phy(6) && hardware.phy(10)
                && hardware.auth(7) && hardware.auth(9) && hardware.cipher(4) && hardware.cipher(31) && !hardware.auth(10),
                "Census unions every adapter's PHY, auth and cipher bits");
        check(WindowsWifi.parseHardware("hw\t0\n").interfaces == 0 && !WindowsWifi.parseHardware("hw\t0\n").present(),
                "Zero adapters is real absence");
        check(WindowsWifi.parseHardware(null) == null && WindowsWifi.parseHardware("hw\t2\ncap\t1\t1\t1\n") == null
                && WindowsWifi.parseHardware("hw\t1\ncap\t-1\t0\t0\n") == null
                && WindowsWifi.parseHardware("hw\t65\n") == null, "Malformed census is not hardware");
        // Per-interface profile listing: an unreadable interface or list is unknown (null).
        java.util.Map<Integer, List<String>> listing = WindowsWifi.parseInterfaceProfiles(
                "12\t1\tGesher-s-a\tOther\n14\t1\n15\t0\n0\t0\n", "Gesher-");
        check(listing.get(12).equals(List.of("Gesher-s-a")) && listing.get(14).isEmpty()
                && listing.containsKey(15) && listing.get(15) == null && listing.containsKey(0) && listing.get(0) == null,
                "Each interface's list is read, empty or unknown");
        check(WindowsWifi.parseInterfaceProfiles(null, "Gesher-") == null
                && WindowsWifi.parseInterfaceProfiles("x\t1\n", "Gesher-") == null
                && WindowsWifi.parseInterfaceProfiles("12\n", "Gesher-") == null
                && WindowsWifi.parseInterfaceProfiles("", "Gesher-").isEmpty(),
                "No enumeration or a malformed listing is unknown; no interfaces is empty");
        // A WLAN service that hangs in its census does not hold the caller (boot) past the bound.
        CountDownLatch hung = new CountDownLatch(1);
        AtomicInteger reads = new AtomicInteger();
        WindowsWifi.Census census = new WindowsWifi.Census(() -> {
            reads.incrementAndGet();
            try { hung.await(); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
            return hardware;
        });
        long started = System.nanoTime();
        check(census.get(100) == WindowsWifi.NO_HARDWARE && System.nanoTime() - started < TimeUnit.SECONDS.toNanos(2),
                "A census past its bound is no hardware, without waiting for Windows");
        // Only the first (boot) caller waits: later callers are not held by the hung probe.
        started = System.nanoTime();
        check(census.get(5_000) == WindowsWifi.NO_HARDWARE && System.nanoTime() - started < TimeUnit.SECONDS.toNanos(1),
                "A later caller does not wait for a hung census again");
        // The probe thread may not have run yet on a loaded host: wait until it is in the census.
        long probing = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (reads.get() == 0 && System.nanoTime() < probing) Thread.sleep(1);
        check(census.get(50) == WindowsWifi.NO_HARDWARE && reads.get() == 1, "A hung census is not started twice");
        hung.countDown();
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
        while (census.get(50) != hardware && System.nanoTime() < deadline) Thread.sleep(5);
        check(census.get(0) == hardware && reads.get() == 1, "The late census result is kept for later callers");
        check(new WindowsWifi.Census(() -> hardware).get(1_000) == hardware, "A prompt census is reported");
        // A caller arriving while a healthy census still runs (within the first bound) gets its result.
        CountDownLatch slow = new CountDownLatch(1);
        AtomicInteger slowReads = new AtomicInteger();
        WindowsWifi.Census healthy = new WindowsWifi.Census(() -> {
            slowReads.incrementAndGet();
            try { slow.await(); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
            return hardware;
        });
        WindowsWifi.Hardware[] boot = new WindowsWifi.Hardware[1];
        Thread first = new Thread(() -> boot[0] = healthy.get(5_000));
        first.start();
        while (slowReads.get() == 0) Thread.sleep(1);
        Thread finisher = new Thread(() -> {
            try { Thread.sleep(200); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
            slow.countDown();
        });
        finisher.start();
        check(healthy.get(5_000) == hardware, "A caller during a healthy census gets its result, not no hardware");
        first.join(5_000);
        check(boot[0] == hardware && slowReads.get() == 1, "The healthy census runs once for both callers");
        // A later caller waits only for the rest of the first caller's bound.
        CountDownLatch stuck = new CountDownLatch(1);
        WindowsWifi.Census hanging = new WindowsWifi.Census(() -> {
            try { stuck.await(); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
            return hardware;
        });
        Thread bootCaller = new Thread(() -> hanging.get(400));
        long bootStarted = System.nanoTime();
        bootCaller.start();
        Thread.sleep(100);
        check(hanging.get(5_000) == WindowsWifi.NO_HARDWARE
                && System.nanoTime() - bootStarted < TimeUnit.MILLISECONDS.toNanos(1_500),
                "A caller during a hung census waits no longer than the first bound");
        stuck.countDown();
        bootCaller.join(5_000);
        Recorder recorder = new Recorder();
        ((List<WindowsWifi.Events>) get("EVENTS")).add(recorder);
        set("sWatching", true);
        String enabled = "ok\t1\n" + IFACE + "4e6574" + END + BSS;
        answer = enabled; idle();
        publish(WindowsWifi.parse(enabled, 10_000_000));
        set("sPublishedState", WifiManager.WIFI_STATE_DISABLED);
        set("sOwnScanNanos", 0L);
        check(WindowsWifi.onEvents(WindowsWifi.EVENT_STATE), "Watcher continues after a state event");
        idle(); await(recorder.events, 2);
        check(recorder.events.contains("state 1>3") && recorder.events.contains("connection")
                && !recorder.events.contains("scan"), "Radio event re-reads and reports the observed transition only");
        recorder.events.clear();
        check(WindowsWifi.onEvents(WindowsWifi.EVENT_STATE), "Repeated state event accepted"); idle(); Thread.sleep(50);
        check(!recorder.events.toString().contains("state"), "An unchanged state is not a transition");
        recorder.events.clear();
        check(WindowsWifi.onEvents(WindowsWifi.EVENT_SCAN), "External scan event accepted"); idle(); await(recorder.events, 1);
        check(recorder.events.equals(List.of("scan")), "Windows-initiated scan completion is published once after a fresh read");
        recorder.events.clear();
        set("sOwnScanNanos", System.nanoTime());
        check(WindowsWifi.onEvents(WindowsWifi.EVENT_SCAN), "Own scan echo accepted"); idle(); Thread.sleep(50);
        check(recorder.events.isEmpty(), "The completion of a requested scan is not published twice");
        set("sOwnScanNanos", 0L);
        answer = "error";
        check(WindowsWifi.onEvents(WindowsWifi.EVENT_SCAN | WindowsWifi.EVENT_STATE), "Event before failed read"); idle(); Thread.sleep(50);
        check(!recorder.events.contains("scan") && !recorder.events.toString().contains("state"),
                "A failed read reports neither fresh results nor a state change");
        answer = enabled;
        publish(WindowsWifi.parse(enabled, 10_000_000));
        check(WindowsWifi.bandObserved(4900, 5900) && !WindowsWifi.bandObserved(5925, 7125),
                "Band evidence comes from observed BSS frequencies only");
        check(WindowsWifi.onEvents(0), "Idle poll keeps watching");
        WindowsWifi.stopWatching();
        check(!WindowsWifi.onEvents(WindowsWifi.EVENT_STATE), "Stopped watcher declines further deliveries");
        ((List<WindowsWifi.Events>) get("EVENTS")).remove(recorder);
    }
    public static void profiles() {
        byte[] ssid = "<Net>&\u05e9".getBytes(StandardCharsets.UTF_8);
        String psk = WlanProfiles.xml("Gesher-r-1", ssid, false,
                android.net.wifi.WifiConfiguration.SECURITY_TYPE_PSK, "\"pa<ss>&'\"word\"", false);
        check(psk != null && psk.contains("<hex>3C4E65743E26D7A9</hex>") && psk.contains("<authentication>WPA2PSK</authentication>")
                && psk.contains("<keyMaterial>pa&lt;ss&gt;&amp;&apos;&quot;word</keyMaterial>")
                && psk.contains("<connectionMode>manual</connectionMode>") && !psk.contains("<Net>"),
                "SSID bytes as hex, escaped passphrase, manual mode");
        String sae = WlanProfiles.xml("Gesher-s-1", ssid, true, android.net.wifi.WifiConfiguration.SECURITY_TYPE_SAE,
                "\"long-sae-password\"", true);
        check(sae != null && sae.contains("WPA3SAE") && sae.contains("<nonBroadcast>true</nonBroadcast>")
                && sae.contains("<connectionMode>auto</connectionMode>"), "SAE, hidden and auto-connect");
        check(WlanProfiles.xml("Gesher-r-1", ssid, false, android.net.wifi.WifiConfiguration.SECURITY_TYPE_OWE, null, false)
                .contains("<authentication>OWE</authentication>"), "OWE profile");
        check(WlanProfiles.xml("Gesher-r-1", ssid, false, android.net.wifi.WifiConfiguration.SECURITY_TYPE_EAP, null, false) == null
                && WlanProfiles.xml("Gesher-r-1", ssid, false, android.net.wifi.WifiConfiguration.SECURITY_TYPE_WEP, "\"k\"", false) == null,
                "Enterprise and WEP are not expressed");
        check(WlanProfiles.xml("Gesher-r-1", ssid, false, android.net.wifi.WifiConfiguration.SECURITY_TYPE_PSK, "\"short\"", false) == null
                && WlanProfiles.xml("Gesher-r-1", ssid, false, android.net.wifi.WifiConfiguration.SECURITY_TYPE_PSK, "\"ok\u0001password\"", false) == null
                && WlanProfiles.xml("Gesher-r-1", ssid, false, android.net.wifi.WifiConfiguration.SECURITY_TYPE_OPEN, "\"secret\"", false) == null,
                "Invalid passphrases and open networks with keys are refused");
        check(WlanProfiles.xml("Other", ssid, false, android.net.wifi.WifiConfiguration.SECURITY_TYPE_OPEN, null, false) == null
                && WlanProfiles.xml("Gesher-r-1", new byte[33], false, android.net.wifi.WifiConfiguration.SECURITY_TYPE_OPEN, null, false) == null,
                "Only Gesher profile names and valid SSID lengths");
        check(WlanProfiles.xml("Gesher-r-1", ssid, false, android.net.wifi.WifiConfiguration.SECURITY_TYPE_PSK,
                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", false).contains("networkKey"),
                "A 64-hex PSK is a raw network key");
    }
    public static void run(boolean nativeBoundary) throws Exception {
        parser(); profiles(); if (nativeBoundary) { host(); events(); }
        // IP provisioning for a specifier connection: a DHCP-style IPv4 address, never an
        // automatic 169.254/16 address or IPv6 alone.
        try {
            check(WindowsWifi.provisioned(java.net.InetAddress.getByName("192.168.4.20")), "A DHCP IPv4 address is provisioning");
            check(!WindowsWifi.provisioned(java.net.InetAddress.getByName("169.254.10.20")), "An APIPA address is not provisioning");
            check(!WindowsWifi.provisioned(java.net.InetAddress.getByName("fe80::1")), "IPv6 link-local is not IPv4 provisioning");
        } catch (java.net.UnknownHostException impossible) {
            throw new AssertionError(impossible);
        }
        System.out.println("WindowsWifiFixture all passed: " + checks + "; nativeBoundary=" + nativeBoundary);
    }
}
