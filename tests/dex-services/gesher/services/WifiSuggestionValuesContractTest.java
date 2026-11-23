/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.MacAddress;
import android.net.wifi.WifiEnterpriseConfig;
import android.net.wifi.WifiNetworkSuggestion;
import android.net.wifi.WifiSsid;
import android.net.wifi.hotspot2.PasspointConfiguration;
import android.net.wifi.hotspot2.pps.HomeSp;
import android.net.wifi.hotspot2.pps.Credential;
import android.net.wifi.hotspot2.pps.Credential.CertificateCredential;
import android.net.wifi.hotspot2.pps.Credential.SimCredential;
import android.net.wifi.hotspot2.pps.Credential.UserCredential;
import android.os.Build;
import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.ParcelUuid;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.security.MessageDigest;
import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;

/** SDK36-only public callers; reflection reaches genuine hidden value dependencies.
 * No Wi-Fi manager, network admission, connection, provisioning or SIM service is invoked.
 */
public final class WifiSuggestionValuesContractTest {
    private static int checks;
    private static final int SENTINEL = 0x56a02391;
    private static final ParcelUuid GROUP = ParcelUuid.fromString("12345678-1234-5678-9abc-123456789abc");
    private interface Action { void run() throws Exception; }
    private interface Security { void apply(WifiNetworkSuggestion.Builder builder) throws Exception; }

    private static void check(boolean condition, String label) {
        if (!condition) throw new AssertionError(label);
        checks++;
    }
    private static void expect(Class<? extends Throwable> type, Action action, String label)
            throws Exception {
        try { action.run(); }
        catch (Throwable failure) {
            while (failure instanceof InvocationTargetException)
                failure = ((InvocationTargetException) failure).getCause();
            if (!type.isInstance(failure)) throw new AssertionError(label, failure);
            checks++;
            return;
        }
        throw new AssertionError(label + ": expected " + type.getSimpleName());
    }
    private static Object call(Object target, String name, Class<?>[] types, Object... args)
            throws Exception {
        return target.getClass().getMethod(name, types).invoke(target, args);
    }
    private static Object get(Object target, String name) throws Exception {
        return call(target, name, new Class<?>[0]);
    }
    private static void set(Object target, String name, Class<?> type, Object value) throws Exception {
        call(target, name, new Class<?>[]{type}, value);
    }
    private static boolean valid(Object value) throws Exception { return (Boolean) get(value, "validate"); }
    private static Object hidden(String name) throws Exception {
        return Class.forName("android.net.wifi.hotspot2.pps." + name, true,
                WifiSuggestionValuesContractTest.class.getClassLoader()).getConstructor().newInstance();
    }
    private static String repeat(String value, int count) {
        StringBuilder result = new StringBuilder();
        for (int i = 0; i < count; i++) result.append(value);
        return result.toString();
    }
    private static WifiNetworkSuggestion.Builder network() {
        return new WifiNetworkSuggestion.Builder().setSsid("suggestion-test");
    }
    private static HomeSp home() {
        HomeSp value = new HomeSp();
        value.setFqdn("wifi.example.test");
        value.setFriendlyName("Public value fixture");
        value.setMatchAllOis(new long[]{1, 0xffffffffffL});
        value.setMatchAnyOis(new long[]{2, 3});
        value.setRoamingConsortiumOis(new long[]{4, 5});
        value.setOtherHomePartnersList(Arrays.asList("partner.example.test", "second.example.test"));
        return value;
    }
    private static UserCredential user() {
        UserCredential value = new UserCredential();
        value.setUsername("public-test-user");
        value.setPassword("public-test-password");
        value.setEapType(21);
        value.setNonEapInnerMethod("PAP");
        return value;
    }
    private static SimCredential sim() {
        SimCredential value = new SimCredential();
        value.setImsi("123456789012345");
        value.setEapType(18);
        return value;
    }
    private static Credential userCredential() {
        Credential value = new Credential();
        value.setRealm("example.test");
        value.setUserCredential(user());
        value.setMinimumTlsVersion(WifiEnterpriseConfig.TLS_V1_2);
        return value;
    }
    private static PasspointConfiguration passpoint() {
        PasspointConfiguration value = new PasspointConfiguration();
        value.setHomeSp(home());
        value.setCredential(userCredential());
        value.setSubscriptionExpirationTimeInMillis(1700000123456L);
        value.setDecoratedIdentityPrefix("example.test!");
        return value;
    }
    private static Credential certificateCredential() throws Exception {
        Credential value = new Credential();
        value.setRealm("certificate.example.test");
        CertificateCredential certificate = new CertificateCredential();
        certificate.setCertType("x509v3");
        certificate.setCertSha256Fingerprint(MessageDigest.getInstance("SHA-256")
                .digest(WifiSuggestionCertificates.client().getEncoded()));
        value.setCertCredential(certificate);
        value.setCaCertificate(WifiSuggestionCertificates.ca());
        value.setClientPrivateKey(WifiSuggestionCertificates.key());
        value.setClientCertificateChain(new X509Certificate[]{WifiSuggestionCertificates.client()});
        value.setMinimumTlsVersion(WifiEnterpriseConfig.TLS_V1_3);
        return value;
    }

    private static void creatorsAndConstants() throws Exception {
        check(WifiNetworkSuggestion.class.getField("RANDOMIZATION_PERSISTENT").getInt(null)
                == WifiNetworkSuggestion.RANDOMIZATION_PERSISTENT, "Persistent SDK constant");
        check(WifiNetworkSuggestion.class.getField("RANDOMIZATION_NON_PERSISTENT").getInt(null)
                == WifiNetworkSuggestion.RANDOMIZATION_NON_PERSISTENT, "Non-persistent SDK constant");
        Parcelable.Creator<?>[] creators = {WifiNetworkSuggestion.CREATOR, PasspointConfiguration.CREATOR,
                HomeSp.CREATOR, Credential.CREATOR, UserCredential.CREATOR,
                CertificateCredential.CREATOR, SimCredential.CREATOR};
        for (Parcelable.Creator<?> creator : creators) {
            check(creator != null && creator.newArray(0).length == 0 && creator.newArray(3).length == 3,
                    "Creator array allocation");
            expect(NegativeArraySizeException.class, () -> creator.newArray(-1), "Negative creator size");
        }
    }

    private static void suggestionDefaultsAndOwnership() throws Exception {
        WifiNetworkSuggestion open = network().build();
        check(open.getSsid().equals("suggestion-test") && open.getWifiSsid().equals(
                WifiSsid.fromBytes("suggestion-test".getBytes("UTF-8"))), "SSID representations");
        check(open.getBssid() == null && open.getPassphrase() == null && open.getPasspointConfig() == null,
                "Default nullable credentials");
        check(open.getEnterpriseConfig() == null && !open.isEnhancedOpen() && !open.isHiddenSsid(),
                "Default open security");
        check(!open.isAppInteractionRequired() && !open.isUserInteractionRequired()
                && !open.isCredentialSharedWithUser() && open.isInitialAutojoinEnabled(), "Default interaction");
        check(!open.isMetered() && !open.isUntrusted() && !open.isRestricted() && !open.isCarrierMerged(),
                "Default capability requests");
        check(open.getPriority() == -1 && open.getPriorityGroup() == 0 && open.getSubscriptionId() == -1
                && open.getSubscriptionGroup() == null, "Default selection metadata");
        check(open.isWifi7Enabled() && open.getMacRandomizationSetting() == 0 && open.describeContents() == 0,
                "Default Wi-Fi7, randomization and content flags");
        check(open.equals(open) && open.equals(network().build())
                && open.hashCode() == network().build().hashCode(), "Default equality and hash");
        check(!open.equals(null) && !open.equals("test") && open.toString().contains("suggestion-test"),
                "Equality boundary and diagnostics");

        byte[] bytes = {(byte) 0xff, 0, 1};
        WifiSsid input = WifiSsid.fromBytes(bytes);
        WifiNetworkSuggestion.Builder builder = new WifiNetworkSuggestion.Builder();
        check(builder.setWifiSsid(input) == builder, "SSID setter fluent return");
        bytes[0] = 1;
        WifiNetworkSuggestion binary = builder.build();
        check(binary.getSsid() == null && Arrays.equals(binary.getWifiSsid().getBytes(), new byte[]{-1, 0, 1}),
                "Binary SSID ownership and nullable UTF-8 representation");
        builder.setSsid("replacement");
        check(Arrays.equals(binary.getWifiSsid().getBytes(), new byte[]{-1, 0, 1}), "Builder reuse SSID isolation");

        WifiEnterpriseConfig enterprise = WifiSuggestionCertificates.enterprise(false);
        builder = network().setWpa2EnterpriseConfig(enterprise);
        enterprise.setIdentity("caller-mutated");
        WifiNetworkSuggestion first = builder.build();
        check(first.getEnterpriseConfig() != enterprise
                && first.getEnterpriseConfig().getIdentity().equals("suggestion-public-test"), "Enterprise setter copy");
        first.getEnterpriseConfig().setIdentity("getter-mutated");
        check(builder.build().getEnterpriseConfig().getIdentity().equals("getter-mutated"),
                "Enterprise getter and reused Builder retain AOSP aliasing");

        PasspointConfiguration source = passpoint();
        builder = new WifiNetworkSuggestion.Builder().setPasspointConfig(source);
        source.getHomeSp().setFqdn("changed.example.test");
        first = builder.build();
        check(first.getPasspointConfig() != source && first.getPasspointConfig().getHomeSp() != source.getHomeSp()
                && first.getPasspointConfig().getHomeSp().getFqdn().equals("wifi.example.test"), "Passpoint setter copy");
        first.getPasspointConfig().getHomeSp().setFriendlyName("getter-mutated");
        check(builder.build().getPasspointConfig().getHomeSp().getFriendlyName().equals("getter-mutated"),
                "Passpoint getter and reused Builder retain AOSP aliasing");
        check(first.getSsid() == null && first.getWifiSsid() == null, "Passpoint has no SSID");
    }

    private static void suggestionSecurityAndErrors() throws Exception {
        Security[] kinds = {
            b -> b.setIsEnhancedOpen(true), b -> b.setWpa2Passphrase("password2"),
            b -> b.setWpa3Passphrase("password3"), b -> b.setWapiPassphrase("password4"),
            b -> b.setWpa2EnterpriseConfig(WifiSuggestionCertificates.enterprise(false)),
            b -> b.setWpa3EnterpriseStandardModeConfig(WifiSuggestionCertificates.enterprise(false)),
            b -> b.setWapiEnterpriseConfig(WifiSuggestionCertificates.enterprise(false)),
            b -> b.setPasspointConfig(passpoint())
        };
        for (int left = 0; left < kinds.length; left++) {
            for (int right = left + 1; right < kinds.length; right++) {
                WifiNetworkSuggestion.Builder b = network();
                kinds[left].apply(b); kinds[right].apply(b);
                expect(IllegalStateException.class, () -> b.build(), "Security exclusivity " + left + "/" + right);
            }
        }
        check(network().setIsEnhancedOpen(true).build().isEnhancedOpen(), "Enhanced open selection");
        check(network().setWpa2Passphrase("a").build().getPassphrase().equals("a"), "No invented PSK minimum");
        check(network().setWpa3Passphrase("b").setIsWpa3SaeH2eOnlyModeEnabled(true).build()
                .getPassphrase().equals("b"), "SAE H2E selection");
        check(network().setWapiPassphrase("c").build().getPassphrase().equals("c"), "WAPI passphrase selection");
        check(network().setWpa2Passphrase("").build().getPassphrase() == null, "Empty passphrase selects open");
        check(network().setWpa3EnterpriseConfig(WifiSuggestionCertificates.enterprise(false)).build()
                .getEnterpriseConfig() != null, "Deprecated WPA3 auto standard selection");
        check(network().setWpa3EnterpriseConfig(WifiSuggestionCertificates.enterprise(true)).build()
                .getEnterpriseConfig().getClientCertificate() != null, "Deprecated WPA3 auto Suite-B selection");
        check(network().setWpa3Enterprise192BitModeConfig(WifiSuggestionCertificates.enterprise(true)).build()
                .getEnterpriseConfig().getCaCertificate() != null, "Explicit WPA3 192 real certificates");
        check(network().setWapiEnterpriseConfig(WifiSuggestionCertificates.enterprise(false)).build()
                .getEnterpriseConfig() != null, "WAPI enterprise value");
        expect(IllegalArgumentException.class, () -> network().setWpa3Enterprise192BitModeConfig(
                WifiSuggestionCertificates.enterprise(false)), "192 requires TLS");
        WifiEnterpriseConfig incomplete = new WifiEnterpriseConfig();
        incomplete.setEapMethod(WifiEnterpriseConfig.Eap.TLS);
        expect(IllegalArgumentException.class, () -> network().setWpa3Enterprise192BitModeConfig(incomplete),
                "192 requires real certificates");
        expect(IllegalArgumentException.class, () -> network().setWpa2EnterpriseConfig(incomplete),
                "WPA2 mandatory server certificate validation");
        expect(IllegalArgumentException.class, () -> network().setWpa3EnterpriseStandardModeConfig(incomplete),
                "WPA3 mandatory server certificate validation");

        expect(IllegalStateException.class, () -> new WifiNetworkSuggestion.Builder().build(), "Missing SSID");
        expect(IllegalStateException.class, () -> network().setSsid("").build(), "Empty SSID at build");
        expect(NullPointerException.class, () -> network().setSsid(null), "Null SSID");
        expect(IllegalArgumentException.class, () -> network().setSsid("\ud800"), "Malformed UTF-16 SSID");
        expect(NullPointerException.class, () -> network().setWifiSsid(null), "Null WifiSsid");
        expect(IllegalArgumentException.class, () -> network().setWifiSsid(WifiSsid.fromBytes(new byte[0])), "Empty WifiSsid");
        expect(NullPointerException.class, () -> network().setBssid(null), "Null BSSID");
        for (String invalid : new String[]{"00:00:00:00:00:00", "ff:ff:ff:ff:ff:ff"}) {
            expect(IllegalStateException.class, () -> network().setBssid(MacAddress.fromString(invalid)).build(),
                    "Reserved BSSID " + invalid);
        }
        check(network().setBssid(MacAddress.fromString("01:00:00:00:00:01")).build().getBssid() != null,
                "No invented multicast BSSID rejection");
        for (int mode = 0; mode < 3; mode++) {
            final int selected = mode;
            expect(NullPointerException.class, () -> password(network(), selected, null), "Null password " + mode);
            expect(IllegalArgumentException.class, () -> password(network(), selected, "non-ascii-\u00e9"),
                    "Non-ASCII password " + mode);
        }
        expect(NullPointerException.class, () -> network().setWpa2EnterpriseConfig(null), "Null WPA2 enterprise");
        expect(NullPointerException.class, () -> network().setWpa3EnterpriseConfig(null), "Null WPA3 enterprise");
        expect(NullPointerException.class, () -> network().setWpa3EnterpriseStandardModeConfig(null), "Null WPA3 standard");
        expect(NullPointerException.class, () -> network().setWpa3Enterprise192BitModeConfig(null), "Null WPA3 192");
        expect(NullPointerException.class, () -> network().setWapiEnterpriseConfig(null), "Null WAPI enterprise");
        expect(NullPointerException.class, () -> network().setPasspointConfig(null), "Null Passpoint");
        expect(IllegalArgumentException.class, () -> network().setPasspointConfig(new PasspointConfiguration()),
                "Invalid Passpoint rejected before build");
        expect(IllegalStateException.class, () -> network().setPasspointConfig(passpoint()).build(), "Passpoint and SSID");
        expect(IllegalStateException.class, () -> new WifiNetworkSuggestion.Builder().setPasspointConfig(passpoint())
                .setIsHiddenSsid(true).build(), "Hidden Passpoint");
        expect(IllegalStateException.class, () -> network().setIsWpa3SaeH2eOnlyModeEnabled(true).build(), "H2E requires SAE");
    }
    private static void password(WifiNetworkSuggestion.Builder builder, int mode, String value) {
        if (mode == 0) builder.setWpa2Passphrase(value);
        else if (mode == 1) builder.setWpa3Passphrase(value);
        else builder.setWapiPassphrase(value);
    }

    private static void suggestionFlags(boolean pinned) throws Exception {
        WifiNetworkSuggestion.Builder b = network();
        check(b.setBssid(MacAddress.fromString("02:11:22:33:44:55")) == b, "BSSID fluent return");
        check(b.setWpa2Passphrase("flag-password") == b && b.setPriority(42) == b && b.setPriorityGroup(-7) == b,
                "Security and priorities fluent return");
        check(b.setIsAppInteractionRequired(true) == b && b.setIsUserInteractionRequired(true) == b
                && b.setIsHiddenSsid(true) == b && b.setIsMetered(true) == b, "Flag fluent return");
        check(b.setIsInitialAutojoinEnabled(false) == b && b.setCredentialSharedWithUser(true) == b
                && b.setMacRandomizationSetting(1) == b && b.setWifi7Enabled(false) == b, "Selection fluent return");
        check(b.setSubscriptionId(6) == b, "Subscription fluent return");
        WifiNetworkSuggestion value = b.build();
        check(value.getPriority() == 42 && value.getPriorityGroup() == -7 && value.getSubscriptionId() == 6,
                "Priority and subscription values");
        check(value.isAppInteractionRequired() && value.isUserInteractionRequired() && value.isHiddenSsid()
                && value.isMetered() && !value.isInitialAutojoinEnabled() && value.isCredentialSharedWithUser(), "Flags retained");
        check(value.getMacRandomizationSetting() == 1 && !value.isWifi7Enabled()
                && value.getBssid().toString().equals("02:11:22:33:44:55"), "Privacy and BSSID values");
        check(!value.toString().contains("flag-password"), "Diagnostics omit secret");
        WifiNetworkSuggestion sameIdentity = network().setWpa2Passphrase("other-password")
                .setBssid(MacAddress.fromString("02:11:22:33:44:55")).setSubscriptionId(6).build();
        check(value.equals(sameIdentity) && value.hashCode() == sameIdentity.hashCode(),
                "Equality ignores password and preference-only flags");
        check(!value.equals(network().setWpa3Passphrase("flag-password").setSubscriptionId(6)
                .setBssid(value.getBssid()).build()), "Equality distinguishes security");
        check(!sameIdentity.equals(network().setWpa2Passphrase("other-password").setBssid(value.getBssid())
                .setSubscriptionId(7).build()), "Equality distinguishes subscription");
        expect(IllegalArgumentException.class, () -> network().setPriority(-1), "Negative priority");
        for (int invalid : new int[]{-1, 2, Integer.MAX_VALUE})
            expect(IllegalArgumentException.class, () -> network().setMacRandomizationSetting(invalid), "Bad randomization");
        expect(IllegalArgumentException.class, () -> network().setSubscriptionId(-1), "Invalid subscription sentinel");
        check(network().setSubscriptionId(-2).build().getSubscriptionId() == -2, "No invented subscription range");
        expect(IllegalArgumentException.class, () -> network().setSubscriptionGroup(null), "Null subscription group");
        check(network().setSubscriptionGroup(GROUP).build().getSubscriptionGroup().equals(GROUP), "Subscription group");
        expect(IllegalStateException.class, () -> network().setSubscriptionId(1).setSubscriptionGroup(GROUP).build(),
                "Subscription id and group mutually exclusive");
        expect(IllegalStateException.class, () -> network().setCredentialSharedWithUser(true).build(), "Open cannot share credentials");
        expect(IllegalStateException.class, () -> network().setIsInitialAutojoinEnabled(false).build(), "Open requires autojoin");
        expect(IllegalStateException.class, () -> network().setWpa2Passphrase("p").setCredentialSharedWithUser(false)
                .setIsInitialAutojoinEnabled(false).build(), "No shared or autojoin route");
        for (boolean restricted : new boolean[]{false, true}) {
            WifiNetworkSuggestion.Builder special = network().setWpa2Passphrase("p");
            if (restricted) special.setRestricted(true); else special.setUntrusted(true);
            WifiNetworkSuggestion selected = special.build();
            check(!selected.isCredentialSharedWithUser() && selected.isRestricted() == restricted
                    && selected.isUntrusted() != restricted, "Restricted/untrusted credential exposure");
            special.setCredentialSharedWithUser(true);
            expect(IllegalStateException.class, () -> special.build(), "Cannot explicitly share special network");
        }
        expect(IllegalStateException.class, () -> network().setCarrierMerged(true).build(), "Carrier requires subscription");
        check(network().setWpa2EnterpriseConfig(WifiSuggestionCertificates.enterprise(false))
                .setSubscriptionId(8).setIsMetered(true).setCarrierMerged(true).build().isCarrierMerged(), "Carrier enterprise metadata");
        WifiNetworkSuggestion.Builder unmetered = network().setWpa2EnterpriseConfig(WifiSuggestionCertificates.enterprise(false))
                .setSubscriptionId(8).setCarrierMerged(true);
        if (pinned) check(unmetered.build().isCarrierMerged(), "R4 permits unmetered carrier metadata");
        else expect(IllegalStateException.class, () -> unmetered.build(), "Stock r1-profile carrier requires metered");
        boolean debug = Build.TYPE.equals("eng") || Build.TYPE.equals("userdebug");
        WifiNetworkSuggestion.Builder carrier = network().setSubscriptionId(8).setIsMetered(true).setCarrierMerged(true);
        if (pinned && debug) check(carrier.build().isCarrierMerged(), "R4 actual debug build permits non-enterprise carrier value");
        else expect(IllegalStateException.class, () -> carrier.build(), "Selected carrier profile requires enterprise");
        System.out.println("Suggestion carrier profile: " + (pinned ? "android16-r4" : "stock-r1-semantics")
                + "; build-type=" + Build.TYPE + "; debug=" + debug);
    }

    private static void homeValuesAndLimits() throws Exception {
        HomeSp empty = new HomeSp();
        check(empty.getFqdn() == null && empty.getFriendlyName() == null && empty.getMatchAllOis() == null
                && empty.getMatchAnyOis() == null && empty.getRoamingConsortiumOis() == null
                && empty.getOtherHomePartnersList().isEmpty(), "HomeSp defaults");
        check(empty.equals(new HomeSp((HomeSp) null)) && empty.describeContents() == 0, "Null HomeSp copy");
        HomeSp value = home();
        HomeSp copy = new HomeSp(value);
        check(value.equals(copy) && value.hashCode() == copy.hashCode() && value.toString().contains("wifi.example.test"),
                "HomeSp value, copy and diagnostics");
        check(!value.equals(null) && !value.equals("home"), "HomeSp equality boundary");
        value.getMatchAllOis()[0] = 99;
        value.getMatchAnyOis()[0] = 98;
        value.getRoamingConsortiumOis()[0] = 97;
        check(copy.getMatchAllOis()[0] == 1 && copy.getMatchAnyOis()[0] == 2 && copy.getRoamingConsortiumOis()[0] == 4,
                "HomeSp copy owns arrays");
        List<String> partners = new ArrayList<>(Arrays.asList("first", "second"));
        value.setOtherHomePartnersList(partners);
        partners.set(0, "caller-mutated");
        check(value.getOtherHomePartnersList().iterator().next().equals("first"), "Partner collection conversion copies");
        value.setOtherHomePartnersList(null);
        check(value.getOtherHomePartnersList().size() == 2, "Null partner setter retains prior value");
        @SuppressWarnings("unchecked") List<String> view = (List<String>) value.getOtherHomePartnersList();
        view.set(0, "view-mutated");
        check(value.getOtherHomePartnersList().iterator().next().equals("view-mutated"), "Partner getter is fixed-size backing view");
        expect(UnsupportedOperationException.class, () -> view.add("third"), "Partner list fixed size");
        Map<String, Long> networks = new HashMap<>(); networks.put("ssid", null);
        set(value, "setHomeNetworkIds", Map.class, networks);
        copy = new HomeSp(value);
        networks.put("later", 1L);
        @SuppressWarnings("unchecked") Map<String, Long> map = (Map<String, Long>) get(copy, "getHomeNetworkIds");
        check(map.size() == 2, "Home network copy is upstream live map view");
        expect(UnsupportedOperationException.class, () -> map.put("third", 2L), "Home network copy map unmodifiable");
        check(valid(home()), "Valid HomeSp");
        check(!valid(empty), "Missing HomeSp identity");
        for (int size : new int[]{254, 255, 256}) {
            HomeSp test = home(); test.setFqdn(repeat("a", size));
            check(valid(test) == (size <= 255), "FQDN bytes boundary " + size);
            test = home(); test.setFriendlyName(repeat("a", size));
            check(valid(test) == (size <= 255), "Friendly bytes boundary " + size);
        }
        HomeSp utf8 = home(); utf8.setFqdn(repeat("\u00e9", 128));
        check(!valid(utf8), "FQDN byte length not UTF-16 count");
        for (int size : new int[]{35, 36, 37}) {
            HomeSp test = home(); test.setMatchAllOis(new long[size]);
            check(valid(test) == (size <= 36), "OI count boundary " + size);
        }
        for (long oi : new long[]{-1, 0, 0xffffffffffL, 0x10000000000L}) {
            HomeSp test = home(); test.setMatchAnyOis(new long[]{oi});
            check(valid(test) == (oi >= 0 && oi <= 0xffffffffffL), "OI range " + oi);
        }
        for (long hessid : new long[]{-1, 0, 0xffffffffffffL, 0x1000000000000L}) {
            HomeSp test = home(); Map<String, Long> ids = new HashMap<>(); ids.put("ssid", hessid);
            set(test, "setHomeNetworkIds", Map.class, ids);
            check(valid(test) == (hessid >= 0 && hessid <= 0xffffffffffffL), "HESSID range " + hessid);
        }
        HomeSp bad = home(); Map<String, Long> ids = new HashMap<>(); ids.put(repeat("x", 33), null);
        set(bad, "setHomeNetworkIds", Map.class, ids); check(!valid(bad), "Home SSID byte limit");
        bad = home(); bad.setOtherHomePartnersList(Collections.nCopies(17, "partner"));
        check(!valid(bad), "Partner count limit");
        set(bad, "setIconUrl", String.class, repeat("x", 2049)); check(!valid(bad), "Icon URL byte limit");
    }

    private static void credentialValuesAndLimits() throws Exception {
        UserCredential value = user(); UserCredential copy = new UserCredential(value);
        check(value.equals(copy) && value.hashCode() == copy.hashCode() && value.describeContents() == 0,
                "User credential copy and hash");
        check(value.getUsername().equals("public-test-user") && value.getPassword().equals("public-test-password")
                && value.getEapType() == 21 && value.getNonEapInnerMethod().equals("PAP"), "User credential fields");
        check(!value.toString().contains("public-test-password") && !value.equals(null) && !value.equals("user"),
                "User secret omission and equality boundary");
        check(new UserCredential((UserCredential) null).equals(new UserCredential()), "Null user copy");
        check(!valid(new UserCredential()) && valid(value), "User validation required fields");
        for (String inner : new String[]{"PAP", "MS-CHAP", "MS-CHAP-V2", "pap", "", null}) {
            copy = user(); copy.setNonEapInnerMethod(inner);
            check(valid(copy) == (inner != null && Arrays.asList("PAP", "MS-CHAP", "MS-CHAP-V2").contains(inner)),
                    "Inner EAP method " + inner);
        }
        for (int size : new int[]{252, 253, 254}) {
            copy = user(); copy.setUsername(repeat("x", size));
            check(valid(copy) == (size <= 253), "Username UTF-8 boundary " + size);
        }
        for (int size : new int[]{254, 255, 256}) {
            copy = user(); copy.setPassword(repeat("x", size));
            check(valid(copy) == (size <= 255), "Password UTF-8 boundary " + size);
        }
        copy = user(); copy.setEapType(18); check(!valid(copy), "User EAP must be TTLS");

        SimCredential sim = sim(); SimCredential simCopy = new SimCredential(sim);
        check(sim.equals(simCopy) && sim.hashCode() == simCopy.hashCode() && sim.describeContents() == 0,
                "SIM value copy");
        check(sim.getImsi().equals("123456789012345") && sim.getEapType() == 18, "SIM value fields");
        check(sim.toString().contains("123456****") && !sim.toString().contains("789012345"), "SIM diagnostic masking");
        check(!sim.equals(null) && !sim.equals("sim") && new SimCredential((SimCredential) null).equals(new SimCredential()),
                "SIM equality boundary and null copy");
        for (String imsi : new String[]{"1", "123456*", "*", "123456789012345", "", "1234567890123456", "123*456", "abc", null}) {
            SimCredential test = sim(); test.setImsi(imsi);
            boolean expected = imsi != null && !imsi.isEmpty() && imsi.length() <= 15 && imsi.matches("[0-9]*\\*?");
            check(valid(test) == expected, "IMSI value syntax " + imsi);
        }
        for (int eap : new int[]{18, 23, 50, 21, -1}) {
            SimCredential test = sim(); test.setEapType(eap);
            check(valid(test) == (eap == 18 || eap == 23 || eap == 50), "SIM EAP " + eap);
        }

        CertificateCredential cert = new CertificateCredential(); cert.setCertType("x509v3");
        byte[] fingerprint = new byte[32]; fingerprint[0] = 7; cert.setCertSha256Fingerprint(fingerprint);
        CertificateCredential certCopy = new CertificateCredential(cert);
        check(cert.getCertType().equals("x509v3") && cert.getCertSha256Fingerprint() == fingerprint
                && cert.equals(certCopy) && cert.hashCode() == certCopy.hashCode() && cert.describeContents() == 0,
                "Certificate value and setter aliasing");
        fingerprint[0] = 8;
        check(certCopy.getCertSha256Fingerprint()[0] == 7, "Certificate copy owns fingerprint");
        check(cert.toString().contains("x509v3") && !cert.equals(null) && !cert.equals("certificate"), "Certificate diagnostics and equality");
        check(new CertificateCredential((CertificateCredential) null).equals(new CertificateCredential()), "Null certificate copy");
        for (int length : new int[]{0, 31, 32, 33}) {
            cert.setCertSha256Fingerprint(new byte[length]); check(valid(cert) == (length == 32), "Certificate fingerprint length " + length);
        }
        cert.setCertType("X509V3"); check(!valid(cert), "Certificate type case-sensitive");

        Credential outer = userCredential(); Credential outerCopy = new Credential(outer);
        check(outer.getRealm().equals("example.test") && outer.getMinimumTlsVersion() == WifiEnterpriseConfig.TLS_V1_2
                && outer.getUserCredential() != null && outer.getCertCredential() == null && outer.getSimCredential() == null,
                "Outer credential fields");
        check(outer.equals(outerCopy) && outer.hashCode() == outerCopy.hashCode()
                && outerCopy.getUserCredential() != outer.getUserCredential() && outer.describeContents() == 0,
                "Outer nested copy");
        check(!outer.equals(null) && !outer.equals("credential") && !outer.toString().contains("public-test-password"),
                "Outer equality and diagnostic boundary");
        check(new Credential((Credential) null).equals(new Credential()) && !valid(new Credential()), "Null outer copy/default invalid");
        check(valid(outer), "Valid TTLS credential without invented certificate requirement");
        outer.setSimCredential(sim()); check(!valid(outer), "Credential kind exclusive");
        outer.setUserCredential(null); check(valid(outer), "SIM credential value validates without installed SIM");
        outer.setRealm(repeat("x", 254)); check(!valid(outer), "Realm bytes limit");
        for (int tls : new int[]{WifiEnterpriseConfig.TLS_V1_0, WifiEnterpriseConfig.TLS_V1_1,
                WifiEnterpriseConfig.TLS_V1_2, WifiEnterpriseConfig.TLS_V1_3}) {
            outer.setMinimumTlsVersion(tls); check(outer.getMinimumTlsVersion() == tls, "Minimum TLS " + tls);
        }
        expect(IllegalArgumentException.class, () -> new Credential().setMinimumTlsVersion(-1), "TLS below range");
        expect(IllegalArgumentException.class, () -> new Credential().setMinimumTlsVersion(4), "TLS above range");

        outer = certificateCredential(); outerCopy = new Credential(outer);
        check(valid(outer) && outer.equals(outerCopy) && outer.getCaCertificate().equals(WifiSuggestionCertificates.ca()),
                "Real X.509/key credential validation");
        check(outerCopy.getClientCertificateChain() != outer.getClientCertificateChain()
                && outerCopy.getClientPrivateKey() == outer.getClientPrivateKey(), "Certificate array copy/key identity");
        outer.getClientCertificateChain()[0] = WifiSuggestionCertificates.ca();
        check(!valid(outer) && valid(outerCopy), "Fingerprint mismatch and certificate copy isolation");
        outerCopy.setClientPrivateKey(null); check(!valid(outerCopy), "Certificate requires private key");
        outerCopy.setCaCertificate(null); check(outerCopy.getCaCertificate() == null, "Nullable CA certificate");
    }

    private static void passpointValuesAndLimits() throws Exception {
        PasspointConfiguration empty = new PasspointConfiguration();
        check(empty.getHomeSp() == null && empty.getCredential() == null && !empty.isOsuProvisioned()
                && empty.getSubscriptionExpirationTimeMillis() == Long.MIN_VALUE && empty.getDecoratedIdentityPrefix() == null,
                "Passpoint defaults");
        check(empty.equals(new PasspointConfiguration((PasspointConfiguration) null)) && empty.describeContents() == 0,
                "Passpoint null copy");
        expect(IllegalStateException.class, () -> empty.getUniqueId(), "Passpoint missing unique identity");
        PasspointConfiguration value = passpoint(); PasspointConfiguration copy = new PasspointConfiguration(value);
        check(value.equals(copy) && value.hashCode() == copy.hashCode() && value.getHomeSp() != copy.getHomeSp()
                && value.getCredential() != copy.getCredential(), "Passpoint nested ownership");
        check(value.getUniqueId().startsWith("wifi.example.test_") && value.getUniqueId().equals(copy.getUniqueId()),
                "Passpoint stable identity");
        copy.getCredential().getUserCredential().setPassword("different");
        check(value.getUniqueId().equals(copy.getUniqueId()) && !value.equals(copy), "Identity excludes credential password");
        check(value.getSubscriptionExpirationTimeMillis() == 1700000123456L
                && value.getDecoratedIdentityPrefix().equals("example.test!"), "Passpoint expiry/decorated identity");
        check(!value.equals(null) && !value.equals("passpoint") && value.toString().contains("wifi.example.test"),
                "Passpoint diagnostics/equality boundary");
        expect(IllegalArgumentException.class, () -> passpoint().setDecoratedIdentityPrefix("missing-delimiter"),
                "Decorated identity requires final delimiter");
        value.setDecoratedIdentityPrefix(""); check(value.getDecoratedIdentityPrefix().isEmpty(), "Empty decorated prefix");
        value.setDecoratedIdentityPrefix(null); check(value.getDecoratedIdentityPrefix() == null, "Null decorated prefix");
        check(valid(value) && !valid(empty) && !(Boolean) get(value, "validateForR2"), "Passpoint R1/R2 validation");

        Map<String, byte[]> roots = new HashMap<>(); roots.put("https://trust.example.test/ca", new byte[32]);
        set(value, "setTrustRootCertList", Map.class, roots);
        String[] aaa = {"aaa.example.test"}; set(value, "setAaaServerTrustedNames", String[].class, aaa);
        Map<String, String> friendly = new HashMap<>(); friendly.put("en", "English name");
        set(value, "setServiceFriendlyNames", Map.class, friendly);
        copy = new PasspointConfiguration(value);
        roots.put("https://trust.example.test/second", new byte[32]);
        @SuppressWarnings("unchecked") Map<String, byte[]> copiedRoots = (Map<String, byte[]>) get(copy, "getTrustRootCertList");
        check(copiedRoots.size() == 2, "Trust root map copy is upstream live view");
        expect(UnsupportedOperationException.class, () -> copiedRoots.put("third", new byte[32]), "Trust root map copy read-only");
        roots.get("https://trust.example.test/ca")[0] = 9;
        check(copiedRoots.get("https://trust.example.test/ca")[0] == 9, "Trust fingerprint arrays retain upstream aliasing");
        aaa[0] = "changed.example.test"; friendly.put("en", "changed name");
        check(((String[]) get(copy, "getAaaServerTrustedNames"))[0].equals("changed.example.test")
                && ((Map<?, ?>) get(copy, "getServiceFriendlyNames")).get("en").equals("changed name"),
                "AAA/friendly collections retain upstream copy aliasing");

        for (int count : new int[]{15, 16, 17}) {
            PasspointConfiguration test = passpoint();
            String[] names = new String[count]; Arrays.fill(names, "aaa.example.test");
            set(test, "setAaaServerTrustedNames", String[].class, names);
            check(valid(test) == (count <= 16), "AAA entry limit " + count);
            Map<String, byte[]> trust = new HashMap<>();
            for (int i = 0; i < count; i++) trust.put("https://trust.example.test/" + i, new byte[32]);
            test = passpoint(); set(test, "setTrustRootCertList", Map.class, trust);
            check(valid(test) == (count <= 16), "Trust root count " + count);
        }
        PasspointConfiguration bad = passpoint();
        set(bad, "setAaaServerTrustedNames", String[].class, new String[]{repeat("\u00e9", 128)});
        check(!valid(bad), "AAA bytes limit");
        bad = passpoint(); set(bad, "setSubscriptionType", String.class, repeat("x", 256));
        check(!valid(bad), "Subscription type length");
        bad = passpoint(); set(bad, "setTrustRootCertList", Map.class, Collections.singletonMap("url", new byte[31]));
        check(!valid(bad), "Trust root fingerprint size");
        bad = passpoint(); set(bad, "setServiceFriendlyNames", Map.class, Collections.singletonMap("en", null));
        check(!valid(bad), "Friendly name nullable entry rejected");
        bad = passpoint(); bad.setDecoratedIdentityPrefix(repeat("a!", 17));
        check(!valid(bad), "Decorated prefix domain count");
        Object never = hidden("UpdateParameter");
        set(never, "setUpdateIntervalInMinutes", long.class, 0xffffffffL);
        set(value, "setUpdateIdentifier", int.class, 42);
        set(value, "setSubscriptionUpdate", never.getClass(), never);
        check(value.isOsuProvisioned() && (Boolean) get(value, "validateForR2"), "R2 provisioned value and update parameter");

        PasspointConfiguration source = passpoint();
        set(source, "setMacRandomizationEnabled", boolean.class, false);
        WifiNetworkSuggestion suggestion = new WifiNetworkSuggestion.Builder().setPasspointConfig(source)
                .setMacRandomizationSetting(1).setSubscriptionGroup(GROUP).setIsMetered(true).build();
        check((Boolean) get(suggestion.getPasspointConfig(), "isMacRandomizationEnabled")
                && (Boolean) get(suggestion.getPasspointConfig(), "isNonPersistentMacRandomizationEnabled")
                && !(Boolean) get(source, "isMacRandomizationEnabled"), "Passpoint suggestion randomization and source isolation");
        check(get(suggestion.getPasspointConfig(), "getSubscriptionGroup").equals(GROUP)
                && ((Integer) get(suggestion.getPasspointConfig(), "getMeteredOverride")) == 1,
                "Passpoint suggestion subscription/metered propagation");
    }

    private static Object update() throws Exception {
        Object value = hidden("UpdateParameter");
        set(value, "setUpdateIntervalInMinutes", long.class, 60L);
        set(value, "setUpdateMethod", String.class, "OMA-DM-ClientInitiated");
        set(value, "setRestriction", String.class, "HomeSP");
        set(value, "setServerUri", String.class, "https://update.example.test/path");
        set(value, "setUsername", String.class, "public-update-user");
        set(value, "setBase64EncodedPassword", String.class, "cHVibGljLXRlc3Q=");
        set(value, "setTrustRootCertUrl", String.class, "https://update.example.test/ca");
        byte[] fingerprint = new byte[32]; fingerprint[1] = 31;
        set(value, "setTrustRootCertSha256Fingerprint", byte[].class, fingerprint);
        set(value, "setCaCertificate", X509Certificate.class, WifiSuggestionCertificates.ca());
        return value;
    }
    private static Object policy() throws Exception {
        Object value = hidden("Policy"), update = update(), partner = hidden("Policy$RoamingPartner");
        set(value, "setPolicyUpdate", update.getClass(), update);
        set(value, "setMinHomeDownlinkBandwidth", long.class, 123L);
        set(value, "setMinHomeUplinkBandwidth", long.class, 456L);
        set(value, "setMinRoamingDownlinkBandwidth", long.class, 789L);
        set(value, "setMinRoamingUplinkBandwidth", long.class, 1011L);
        set(value, "setMaximumBssLoadValue", int.class, 12);
        set(value, "setExcludedSsidList", String[].class, new String[]{"excluded"});
        Map<Integer, String> ports = new HashMap<>(); ports.put(6, "443"); ports.put(17, "53");
        set(value, "setRequiredProtoPortMap", Map.class, ports);
        set(partner, "setFqdn", String.class, "roaming.example.test");
        set(partner, "setFqdnExactMatch", boolean.class, true);
        set(partner, "setPriority", int.class, 4);
        set(partner, "setCountries", String.class, "US,CA");
        set(value, "setPreferredRoamingPartnerList", List.class, new ArrayList<>(Arrays.asList(partner)));
        return value;
    }
    private static void hiddenDependencyContracts() throws Exception {
        Object update = update(); check(valid(update), "Valid full update parameter");
        Object copy = update.getClass().getConstructor(update.getClass()).newInstance(update);
        check(update.equals(copy) && update.hashCode() == copy.hashCode(), "Update copy/value equality");
        ((byte[]) get(update, "getTrustRootCertSha256Fingerprint"))[0] = 9;
        check(((byte[]) get(copy, "getTrustRootCertSha256Fingerprint"))[0] == 0, "Update copy owns fingerprint");
        check(!update.toString().contains("cHVibGljLXRlc3Q="), "Update diagnostics omit password");
        for (String restriction : new String[]{"HomeSP", "RoamingPartner", "Unrestricted", "bad"}) {
            set(update, "setRestriction", String.class, restriction);
            check(valid(update) == !restriction.equals("bad"), "Update restriction " + restriction);
        }
        update = update(); set(update, "setUpdateMethod", String.class, "SPP-ClientInitiated");
        check(valid(update), "SPP update method");
        set(update, "setUpdateMethod", String.class, "bad"); check(!valid(update), "Unknown update method");
        update = update(); set(update, "setBase64EncodedPassword", String.class, "a");
        check(!valid(update), "Update Base64 validation");
        update = update(); set(update, "setUsername", String.class, repeat("x", 64));
        check(!valid(update), "Update username bytes");
        update = update(); set(update, "setServerUri", String.class, repeat("x", 1024));
        check(!valid(update), "Update server URI bytes");
        update = hidden("UpdateParameter"); check(!valid(update), "Missing update interval");
        set(update, "setUpdateIntervalInMinutes", long.class, 0xffffffffL);
        check(valid(update), "Never-update shortcut");

        Object policy = policy(); check(valid(policy), "Full policy validation");
        copy = policy.getClass().getConstructor(policy.getClass()).newInstance(policy);
        check(policy.equals(copy) && policy.hashCode() == copy.hashCode(), "Policy copy/value equality");
        ((String[]) get(policy, "getExcludedSsidList"))[0] = "changed";
        check(((String[]) get(copy, "getExcludedSsidList"))[0].equals("excluded"), "Policy copy owns SSID array");
        @SuppressWarnings("unchecked") Map<Integer, String> sourcePorts = (Map<Integer, String>) get(policy, "getRequiredProtoPortMap");
        sourcePorts.put(1, "");
        check(((Map<?, ?>) get(copy, "getRequiredProtoPortMap")).size() == 3, "Policy copy port map retains live view");
        @SuppressWarnings("unchecked") List<Object> sourcePartners = (List<Object>) get(policy, "getPreferredRoamingPartnerList");
        set(sourcePartners.get(0), "setPriority", int.class, 99);
        check(((Integer) get(((List<?>) get(copy, "getPreferredRoamingPartnerList")).get(0), "getPriority")) == 99,
                "Policy roaming partner copy retains element aliasing");
        check(!valid(hidden("Policy")), "Policy requires update parameter");
        for (int protocol : new int[]{-1, 0, 255, 256}) {
            policy = policy(); set(policy, "setRequiredProtoPortMap", Map.class, Collections.singletonMap(protocol, "443"));
            check(valid(policy) == (protocol >= 0 && protocol <= 255), "Protocol range " + protocol);
        }
        policy = policy(); set(policy, "setExcludedSsidList", String[].class, new String[]{repeat("x", 33)});
        check(!valid(policy), "Excluded SSID bytes");
        policy = policy(); String[] excluded = new String[129]; Arrays.fill(excluded, "");
        set(policy, "setExcludedSsidList", String[].class, excluded); check(!valid(policy), "Excluded SSID count");
        policy = policy(); set(policy, "setRequiredProtoPortMap", Map.class, Collections.singletonMap(6, repeat("x", 65)));
        check(!valid(policy), "Port list bytes");
        Object partner = hidden("Policy$RoamingPartner"); check(!valid(partner), "Roaming partner required fields");
        set(partner, "setFqdn", String.class, "roam.test"); set(partner, "setCountries", String.class, "US");
        check(valid(partner), "Roaming partner values");
        set(partner, "setCountries", String.class, repeat("x", 256)); check(!valid(partner), "Roaming country bytes");
    }

    private static PasspointConfiguration fullPasspoint() throws Exception {
        PasspointConfiguration value = passpoint();
        value.setCredential(certificateCredential());
        Object policy = policy(), update = update();
        set(value, "setPolicy", policy.getClass(), policy);
        set(value, "setSubscriptionUpdate", update.getClass(), update);
        Map<String, byte[]> roots = new HashMap<>(); byte[] digest = new byte[32]; digest[7] = 23;
        roots.put("https://trust.example.test/root", digest); set(value, "setTrustRootCertList", Map.class, roots);
        set(value, "setAaaServerTrustedNames", String[].class, new String[]{"aaa.example.test", "aaa2.example.test"});
        Map<String, String> names = new HashMap<>(); names.put("en", "English test"); names.put("fr", "French test");
        set(value, "setServiceFriendlyNames", Map.class, names);
        set(value, "setUpdateIdentifier", int.class, 17); set(value, "setCredentialPriority", int.class, 19);
        set(value, "setSubscriptionCreationTimeInMillis", long.class, 1600000000000L);
        set(value, "setSubscriptionType", String.class, "public-test-type");
        set(value, "setUsageLimitUsageTimePeriodInMinutes", long.class, 60L);
        set(value, "setUsageLimitStartTimeInMillis", long.class, 1700000000000L);
        set(value, "setUsageLimitDataLimit", long.class, 123456789L);
        set(value, "setUsageLimitTimeLimitInMinutes", long.class, 90L);
        set(value, "setCarrierId", int.class, 31); set(value, "setSubscriptionId", int.class, 32);
        set(value, "setSubscriptionGroup", ParcelUuid.class, GROUP);
        set(value, "setAutojoinEnabled", boolean.class, false);
        set(value, "setMacRandomizationEnabled", boolean.class, false);
        set(value, "setNonPersistentMacRandomizationEnabled", boolean.class, true);
        set(value, "setMeteredOverride", int.class, 2);
        set(value, "setOemPaid", boolean.class, true); set(value, "setOemPrivate", boolean.class, true);
        set(value, "setCarrierMerged", boolean.class, true);
        set(value.getCredential(), "setCreationTimeInMillis", long.class, 1550000000000L);
        set(value.getCredential(), "setExpirationTimeInMillis", long.class, 1850000000000L);
        set(value.getCredential(), "setCheckAaaServerCertStatus", boolean.class, true);
        Map<String, Long> ids = new HashMap<>(); ids.put("home", 0x112233445566L); ids.put("nullable", null);
        set(value.getHomeSp(), "setHomeNetworkIds", Map.class, ids);
        set(value.getHomeSp(), "setIconUrl", String.class, "https://wifi.example.test/icon");
        return value;
    }
    private static void directWrite(Parcelable value, Parcel parcel) {
        if (value instanceof WifiNetworkSuggestion) ((WifiNetworkSuggestion) value).writeToParcel(parcel, 0);
        else if (value instanceof PasspointConfiguration) ((PasspointConfiguration) value).writeToParcel(parcel, 0);
        else if (value instanceof HomeSp) ((HomeSp) value).writeToParcel(parcel, 0);
        else if (value instanceof Credential) ((Credential) value).writeToParcel(parcel, 0);
        else if (value instanceof UserCredential) ((UserCredential) value).writeToParcel(parcel, 0);
        else if (value instanceof CertificateCredential) ((CertificateCredential) value).writeToParcel(parcel, 0);
        else if (value instanceof SimCredential) ((SimCredential) value).writeToParcel(parcel, 0);
        else value.writeToParcel(parcel, 0);
    }
    private static byte[] bytes(Parcelable value) {
        Parcel parcel = Parcel.obtain();
        try { directWrite(value, parcel); return parcel.marshall(); }
        finally { parcel.recycle(); }
    }
    private static byte[] friendlyBundle(PasspointConfiguration value) throws Exception {
        Parcel parcel = Parcel.obtain();
        try {
            Bundle bundle = new Bundle();
            bundle.putSerializable("serviceFriendlyNames", (HashMap<?, ?>) get(value, "getServiceFriendlyNames"));
            parcel.writeBundle(bundle);
            return parcel.marshall();
        } finally { parcel.recycle(); }
    }
    private static byte[] trustRoots(PasspointConfiguration value) throws Exception {
        Parcel parcel = Parcel.obtain();
        try {
            Method writer = PasspointConfiguration.class.getDeclaredMethod("writeTrustRootCerts", Parcel.class, Map.class);
            writer.setAccessible(true);
            writer.invoke(null, parcel, get(value, "getTrustRootCertList"));
            return parcel.marshall();
        } finally { parcel.recycle(); }
    }
    private static String digest(byte[] value) throws Exception {
        StringBuilder text = new StringBuilder();
        for (byte b : MessageDigest.getInstance("SHA-256").digest(value)) text.append(String.format("%02x", b & 255));
        return text.toString();
    }
    private static void mapDifference(String name, String field, byte[] before, byte[] after) throws Exception {
        int first = 0;
        while (first < Math.min(before.length, after.length) && before[first] == after[first]) first++;
        System.out.println("SUGGESTION_MAP_REENCODING case=" + name + " field=" + field + " first_diff=" + first
                + " before_bytes=" + before.length + " after_bytes=" + after.length
                + " before_sha256=" + digest(before) + " after_sha256=" + digest(after));
    }
    /** Unordered map reconstruction may change encoding without changing the value.
     * Prove each changed component is explained by its parsed map representation,
     * then require the complete adjusted value's bytes to match the complete parsed
     * value. First-write Android reference/candidate bytes are never normalized.
     */
    private static void explainMapReencoding(String name, Parcelable value, Parcelable parsed, byte[] parsedBytes)
            throws Exception {
        PasspointConfiguration originalPasspoint, parsedPasspoint;
        if (value instanceof WifiNetworkSuggestion) {
            originalPasspoint = ((WifiNetworkSuggestion) value).getPasspointConfig();
            parsedPasspoint = ((WifiNetworkSuggestion) parsed).getPasspointConfig();
        } else if (value instanceof PasspointConfiguration) {
            originalPasspoint = (PasspointConfiguration) value;
            parsedPasspoint = (PasspointConfiguration) parsed;
        } else throw new AssertionError(name + " unexpected wire re-encoding outside Passpoint maps");
        check(originalPasspoint != null && parsedPasspoint != null && originalPasspoint.equals(parsedPasspoint),
                name + " complete Passpoint value equality before map analysis");
        PasspointConfiguration explained = new PasspointConfiguration(originalPasspoint);
        int differences = 0;
        HomeSp beforeHome = originalPasspoint.getHomeSp(), afterHome = parsedPasspoint.getHomeSp();
        byte[] before = bytes(beforeHome), after = bytes(afterHome);
        if (!Arrays.equals(before, after)) {
            HomeSp home = new HomeSp(beforeHome);
            Object originalMap = get(beforeHome, "getHomeNetworkIds"), parsedMap = get(afterHome, "getHomeNetworkIds");
            check(Objects.equals(originalMap, parsedMap), name + " HomeSp map content equality");
            set(home, "setHomeNetworkIds", Map.class, parsedMap);
            check(home.equals(beforeHome) && Arrays.equals(bytes(home), after), name + " HomeSp map alone explains component bytes");
            explained.setHomeSp(home);
            mapDifference(name, "HomeSp.homeNetworkIds", before, after);
            differences++;
        }
        Parcelable beforePolicy = (Parcelable) get(originalPasspoint, "getPolicy");
        Parcelable afterPolicy = (Parcelable) get(parsedPasspoint, "getPolicy");
        if (beforePolicy != null && !Arrays.equals(before = bytes(beforePolicy), after = bytes(afterPolicy))) {
            Object policy = beforePolicy.getClass().getConstructor(beforePolicy.getClass()).newInstance(beforePolicy);
            Object ports = get(afterPolicy, "getRequiredProtoPortMap");
            check(Objects.equals(get(beforePolicy, "getRequiredProtoPortMap"), ports), name + " policy map content equality");
            set(policy, "setRequiredProtoPortMap", Map.class, ports);
            check(policy.equals(beforePolicy) && Arrays.equals(bytes((Parcelable) policy), after),
                    name + " policy map alone explains component bytes");
            set(explained, "setPolicy", policy.getClass(), policy);
            mapDifference(name, "Policy.requiredProtoPortMap", before, after);
            differences++;
        }
        if (!Arrays.equals(before = trustRoots(originalPasspoint), after = trustRoots(parsedPasspoint))) {
            set(explained, "setTrustRootCertList", Map.class, get(parsedPasspoint, "getTrustRootCertList"));
            check(Arrays.equals(trustRoots(explained), after), name + " trust root map encoding explanation");
            mapDifference(name, "Passpoint.trustRootCertList", before, after);
            differences++;
        }
        if (!Arrays.equals(before = friendlyBundle(originalPasspoint), after = friendlyBundle(parsedPasspoint))) {
            check(Objects.equals(get(originalPasspoint, "getServiceFriendlyNames"),
                    get(parsedPasspoint, "getServiceFriendlyNames")), name + " friendly map content equality");
            set(explained, "setServiceFriendlyNames", Map.class, get(parsedPasspoint, "getServiceFriendlyNames"));
            check(Arrays.equals(friendlyBundle(explained), after), name + " friendly map serialization explanation");
            mapDifference(name, "Passpoint.serviceFriendlyNames", before, after);
            differences++;
        }
        check(differences > 0 && explained.equals(originalPasspoint)
                && Arrays.equals(bytes(explained), bytes(parsedPasspoint)), name + " only identified maps explain full Passpoint re-encoding");
        Parcelable whole = explained;
        if (value instanceof WifiNetworkSuggestion) {
            WifiNetworkSuggestion suggestion = (WifiNetworkSuggestion) value;
            java.lang.reflect.Field configuration = WifiNetworkSuggestion.class.getField("wifiConfiguration");
            whole = WifiNetworkSuggestion.class.getConstructor(configuration.getType(), PasspointConfiguration.class,
                    boolean.class, boolean.class, boolean.class, boolean.class, int.class).newInstance(
                    configuration.get(value), explained, suggestion.isAppInteractionRequired(),
                    suggestion.isUserInteractionRequired(), suggestion.isCredentialSharedWithUser(),
                    suggestion.isInitialAutojoinEnabled(), suggestion.getPriorityGroup());
        }
        check(whole.equals(value) && Arrays.equals(bytes(whole), parsedBytes),
                name + " only identified maps explain complete outer wire difference");
    }
    private static void wire(String name, Parcelable value, Parcelable.Creator<?> creator, boolean equalHash)
            throws Exception {
        byte[] wire = bytes(value);
        Parcel parcel = Parcel.obtain();
        Parcelable copied;
        try {
            directWrite(value, parcel); int end = parcel.dataPosition(); parcel.writeInt(SENTINEL);
            parcel.setDataPosition(0); copied = (Parcelable) creator.createFromParcel(parcel);
            check(parcel.dataPosition() == end && parcel.readInt() == SENTINEL, name + " exact cursor and sentinel");
            check(copied.getClass() == value.getClass() && copied.equals(value) && value.equals(copied), name + " native value roundtrip");
            byte[] parsedBytes = bytes(copied);
            if (Arrays.equals(wire, parsedBytes)) check(true, name + " full wire roundtrip");
            else {
                explainMapReencoding(name, value, copied, parsedBytes);
                Parcel settled = Parcel.obtain();
                try {
                    directWrite(copied, settled); settled.setDataPosition(0);
                    Parcelable second = (Parcelable) creator.createFromParcel(settled);
                    check(settled.dataAvail() == 0 && copied.equals(second)
                            && Arrays.equals(parsedBytes, bytes(second)), name + " parsed map encoding stabilizes");
                } finally { settled.recycle(); }
            }
            check(copied.describeContents() == 0, name + " content flags");
            if (equalHash) check(copied.hashCode() == value.hashCode(), name + " hash roundtrip");
        } finally { parcel.recycle(); }
        parcel = Parcel.obtain();
        try {
            parcel.writeParcelable(value, 0); parcel.setDataPosition(0);
            Object outer = parcel.readParcelable(value.getClass().getClassLoader());
            check(outer != null && outer.getClass() == value.getClass() && outer.equals(value)
                    && parcel.dataAvail() == 0, name + " named Parcelable dispatch");
        } finally { parcel.recycle(); }
        System.out.println("SUGGESTION_WIRE name=" + name + " bytes=" + wire.length + " sha256=" + digest(wire));
    }
    private static Parcelable.Creator<?> creator(Object value) throws Exception {
        return (Parcelable.Creator<?>) value.getClass().getField("CREATOR").get(null);
    }
    private static void parcels() throws Exception {
        wire("open", network().build(), WifiNetworkSuggestion.CREATOR, true);
        wire("owe", network().setIsEnhancedOpen(true).build(), WifiNetworkSuggestion.CREATOR, true);
        wire("wpa2", network().setWpa2Passphrase("public-test-password").setPriority(7).setPriorityGroup(3)
                .setBssid(MacAddress.fromString("02:11:22:33:44:55")).setIsHiddenSsid(true).setIsMetered(true)
                .setIsAppInteractionRequired(true).setIsUserInteractionRequired(true).setSubscriptionId(2)
                .setIsInitialAutojoinEnabled(false).setWifi7Enabled(false).build(), WifiNetworkSuggestion.CREATOR, true);
        wire("wpa3", network().setWpa3Passphrase("public-test-password").setIsWpa3SaeH2eOnlyModeEnabled(true)
                .setMacRandomizationSetting(1).setSubscriptionGroup(GROUP).build(), WifiNetworkSuggestion.CREATOR, true);
        wire("wapi", network().setWapiPassphrase("public-test-password").build(), WifiNetworkSuggestion.CREATOR, true);
        wire("enterprise", network().setWpa2EnterpriseConfig(WifiSuggestionCertificates.enterprise(false))
                .setSubscriptionId(3).setIsMetered(true).setCarrierMerged(true).build(), WifiNetworkSuggestion.CREATOR, true);
        wire("enterprise192", network().setWpa3Enterprise192BitModeConfig(WifiSuggestionCertificates.enterprise(true))
                .build(), WifiNetworkSuggestion.CREATOR, true);
        wire("binary_restricted", new WifiNetworkSuggestion.Builder().setWifiSsid(WifiSsid.fromBytes(new byte[]{-1, 0, 1}))
                .setWpa2Passphrase("public-test-password").setRestricted(true).setUntrusted(true).build(), WifiNetworkSuggestion.CREATOR, true);
        wire("passpoint_suggestion", new WifiNetworkSuggestion.Builder().setPasspointConfig(passpoint())
                .setSubscriptionGroup(GROUP).setMacRandomizationSetting(1).setIsMetered(true).build(), WifiNetworkSuggestion.CREATOR, true);
        wire("passpoint_full_suggestion", new WifiNetworkSuggestion.Builder().setPasspointConfig(fullPasspoint())
                .setSubscriptionId(4).setIsMetered(true).setCarrierMerged(true).build(), WifiNetworkSuggestion.CREATOR, true);
        wire("passpoint_empty", new PasspointConfiguration(), PasspointConfiguration.CREATOR, true);
        wire("passpoint_basic", passpoint(), PasspointConfiguration.CREATOR, true);
        // Upstream map hashes use byte[] identity even though equality compares their contents.
        wire("passpoint_full", fullPasspoint(), PasspointConfiguration.CREATOR, false);
        wire("home", home(), HomeSp.CREATOR, true);
        wire("credential_empty", new Credential(), Credential.CREATOR, true);
        wire("credential_user", userCredential(), Credential.CREATOR, true);
        wire("credential_certificate", certificateCredential(), Credential.CREATOR, false);
        Credential sim = new Credential(); sim.setRealm("sim.example.test"); sim.setSimCredential(sim());
        wire("credential_sim", sim, Credential.CREATOR, true);
        UserCredential user = user();
        set(user, "setMachineManaged", boolean.class, true); set(user, "setSoftTokenApp", String.class, "public.test.token");
        set(user, "setAbleToShare", boolean.class, true);
        wire("user", user, UserCredential.CREATOR, true);
        wire("certificate", certificateCredential().getCertCredential(), CertificateCredential.CREATOR, true);
        wire("sim", sim(), SimCredential.CREATOR, true);
        Parcelable policy = (Parcelable) policy(); wire("policy", policy, creator(policy), true);
        Parcelable partner = (Parcelable) ((List<?>) get(policy, "getPreferredRoamingPartnerList")).get(0);
        wire("roaming_partner", partner, creator(partner), true);
        Parcelable update = (Parcelable) update(); wire("update", update, creator(update), true);
    }

    public static void baselineSuggestionControl() { new WifiNetworkSuggestion.Builder().setSsid("absent").build(); }
    public static void baselinePasspointControl() { new PasspointConfiguration().getHomeSp(); }
    /** Check every nontrivial type boundary before constructing corpus values. */
    public static void verifyGraph() throws Exception {
        ClassLoader loader = WifiSuggestionValuesContractTest.class.getClassLoader();
        Class<?> config = Class.forName("android.net.wifi.WifiConfiguration", false, loader);
        Class<?> security = Class.forName("android.net.wifi.SecurityParams", false, loader);
        Class<?> policy = Class.forName("android.net.wifi.hotspot2.pps.Policy", false, loader);
        Class<?> update = Class.forName("android.net.wifi.hotspot2.pps.UpdateParameter", false, loader);
        Class<?> partner = Class.forName("android.net.wifi.hotspot2.pps.Policy$RoamingPartner", false, loader);
        if (WifiNetworkSuggestion.class.getField("wifiConfiguration").getType() != config
                || WifiNetworkSuggestion.class.getField("passpointConfiguration").getType() != PasspointConfiguration.class
                || config.getField("enterpriseConfig").getType() != WifiEnterpriseConfig.class
                || config.getMethod("getDefaultSecurityParams").getReturnType() != security
                || config.getMethod("setSecurityParams", security).getParameterTypes()[0] != security
                || config.getMethod("getRandomizedMacAddress").getReturnType() != MacAddress.class
                || WifiNetworkSuggestion.Builder.class.getMethod("setPasspointConfig", PasspointConfiguration.class)
                        .getReturnType() != WifiNetworkSuggestion.Builder.class
                || WifiNetworkSuggestion.Builder.class.getMethod("setWpa2EnterpriseConfig", WifiEnterpriseConfig.class)
                        .getParameterTypes()[0] != WifiEnterpriseConfig.class
                || WifiNetworkSuggestion.Builder.class.getMethod("setWifiSsid", WifiSsid.class)
                        .getParameterTypes()[0] != WifiSsid.class
                || WifiNetworkSuggestion.Builder.class.getMethod("setBssid", MacAddress.class)
                        .getParameterTypes()[0] != MacAddress.class
                || WifiNetworkSuggestion.class.getMethod("getWifiSsid").getReturnType() != WifiSsid.class
                || WifiNetworkSuggestion.class.getMethod("getEnterpriseConfig").getReturnType() != WifiEnterpriseConfig.class
                || WifiNetworkSuggestion.class.getMethod("getPasspointConfig").getReturnType() != PasspointConfiguration.class
                || PasspointConfiguration.class.getMethod("getHomeSp").getReturnType() != HomeSp.class
                || PasspointConfiguration.class.getMethod("setHomeSp", HomeSp.class).getParameterTypes()[0] != HomeSp.class
                || PasspointConfiguration.class.getMethod("getCredential").getReturnType() != Credential.class
                || PasspointConfiguration.class.getMethod("setCredential", Credential.class).getParameterTypes()[0] != Credential.class
                || PasspointConfiguration.class.getMethod("getPolicy").getReturnType() != policy
                || PasspointConfiguration.class.getMethod("setPolicy", policy).getParameterTypes()[0] != policy
                || PasspointConfiguration.class.getMethod("getSubscriptionUpdate").getReturnType() != update
                || policy.getMethod("getPolicyUpdate").getReturnType() != update
                || policy.getMethod("setPolicyUpdate", update).getParameterTypes()[0] != update
                || Credential.class.getMethod("getUserCredential").getReturnType() != UserCredential.class
                || Credential.class.getMethod("setUserCredential", UserCredential.class).getParameterTypes()[0] != UserCredential.class
                || Credential.class.getMethod("getCertCredential").getReturnType() != CertificateCredential.class
                || Credential.class.getMethod("setCertCredential", CertificateCredential.class).getParameterTypes()[0] != CertificateCredential.class
                || Credential.class.getMethod("getSimCredential").getReturnType() != SimCredential.class
                || Credential.class.getMethod("setSimCredential", SimCredential.class).getParameterTypes()[0] != SimCredential.class
                || partner.getConstructor(partner).getParameterTypes()[0] != partner)
            throw new AssertionError("Incoherent value dependency identities");
        Class<?> parcelUtil = Class.forName("android.net.wifi.ParcelUtil", false, loader);
        if (parcelUtil.getMethod("readCertificate", Parcel.class).getReturnType() != X509Certificate.class
                || parcelUtil.getMethod("readPrivateKey", Parcel.class).getReturnType() != java.security.PrivateKey.class
                || WifiSuggestionCertificates.class.getDeclaredMethod("enterprise", boolean.class)
                        .getReturnType() != WifiEnterpriseConfig.class)
            throw new AssertionError("Incoherent certificate helper identities");
        System.out.println("Suggestion/Passpoint dependency parameter and return identities verified");
    }
    public static void run(boolean parcel, boolean pinned) throws Exception {
        checks = 0;
        creatorsAndConstants(); suggestionDefaultsAndOwnership(); suggestionSecurityAndErrors(); suggestionFlags(pinned);
        homeValuesAndLimits(); credentialValuesAndLimits(); passpointValuesAndLimits(); hiddenDependencyContracts();
        if (parcel) parcels();
        System.out.println("Wifi suggestion values: " + checks + " checks; parcel=" + parcel + "; pinned=" + pinned);
    }
    public static void main(String[] args) {
        try {
            boolean parcel = Arrays.asList(args).contains("--require-parcel");
            verifyGraph();
            run(parcel, true);
            if (parcel) System.out.println("WIFI_SUGGESTION_NATIVE_PARCEL_REQUIRED_OK");
        } catch (Throwable failure) {
            while (failure instanceof InvocationTargetException)
                failure = ((InvocationTargetException) failure).getCause();
            System.err.println("WIFI_SUGGESTION_CONTRACT_FAILURE");
            failure.printStackTrace(System.err);
            System.exit(1);
        }
    }
}
