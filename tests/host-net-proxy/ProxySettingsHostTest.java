/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net;

import java.net.Proxy;
import java.net.ProxySelector;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.TimeUnit;

/** Production configuration plus real host sockets; Android glue is isolated in run.py. */
public final class ProxySettingsHostTest {
    private static int checks;
    private static final String[] PROPERTIES = {
        "http.proxyHost", "http.proxyPort", "http.nonProxyHosts",
        "https.proxyHost", "https.proxyPort", "https.nonProxyHosts",
        "socksProxyHost", "socksProxyPort", "socksProxyVersion", "socksNonProxyHosts"
    };

    private static void check(boolean condition, String message) {
        checks++;
        if (!condition) throw new AssertionError(message);
    }

    private static void equal(Object actual, Object expected, String message) {
        check(Objects.equals(actual, expected), message + ": expected " + expected + ", got " + actual);
    }

    private static WindowsProxy.Config config(String proxy, String bypass) {
        WindowsProxy.Config value = new WindowsProxy.Config();
        value.proxy = proxy;
        value.bypass = bypass;
        return value;
    }

    private static void noSocks(String message) {
        for (String property : new String[] {
                "socksProxyHost", "socksProxyPort", "socksProxyVersion", "socksNonProxyHosts"}) {
            equal(System.getProperty(property), null, message + " " + property);
        }
    }

    private static Proxy selected(String uri) {
        List<Proxy> result = ProxySelector.getDefault().select(URI.create(uri));
        check(result.size() == 1, "one proxy selection for " + uri);
        return result.get(0);
    }

    private static void parsing() {
        String[][] accepted = {
            {"socks=proxy.example", "socks5://proxy.example:1080"},
            {"socks5=proxy.example:9150", "socks5://proxy.example:9150"},
            {"socks://127.0.0.1", "socks5://127.0.0.1:1080"},
            {"socks5://127.0.0.1:1080", "socks5://127.0.0.1:1080"},
            {" SOCKS5://Proxy.Example:01080/ ", "socks5://Proxy.Example:1080"},
            {"socks5://[::1]", "socks5://[::1]:1080"},
            {"socks=[2001:db8::1]:65535", "socks5://[2001:db8::1]:65535"},
            {"socks5://[::ffff:192.0.2.4]:1", "socks5://[::ffff:192.0.2.4]:1"},
            {"socks5://proxy.example.", "socks5://proxy.example.:1080"},
            {"http://127.0.0.1:8080/", "127.0.0.1:8080"},
            {"[::1]:8080", "[::1]:8080"},
            {" OFF ", "direct"}, {"none", "direct"}, {"default", "windows"}
        };
        for (String[] value : accepted) equal(ProxySetting.parse(value[0]), value[1], "parse " + value[0]);
        for (String value : new String[] {
                "socks5://", "socks5://:1080", "socks5://proxy:", "socks5://proxy:0",
                "socks5://proxy:-1", "socks5://proxy:65536", "socks5://proxy:+1080",
                "socks5://proxy:abc", "socks5://proxy:99999999999999", "socks5://proxy:１０８０",
                "socks5://user:secret@proxy:1080", "socks5://proxy:1080/path",
                "socks5://proxy:1080?query", "socks5://proxy:1080#fragment",
                "socks5://proxy:1080//", "socks5://proxy name:1080", "socks5://a..b:1080",
                "socks5://-bad:1080", "socks5://bad-:1080", "socks5://999.0.0.1:1080",
                "socks5://[::1", "socks5://[::1]suffix:1080", "socks5://[not-ipv6]:1080",
                "socks5://[1:2:3]:1080", "socks5://::1:1080", "socks4://proxy:1080"}) {
            equal(ProxySetting.parse(value), null, "reject " + value);
        }
        equal(ProxySetting.parse(null), null, "null setting");
        System.out.println("PASS proxy parsing: SOCKS aliases, IPv6, defaults, malformed and HTTP compatibility");
    }

    private static void propertiesAndSelection() {
        for (String alias : new String[] {"socks=", "socks5=", "socks://", "socks5://"}) {
            WindowsProxy.Config value = config(alias + "proxy.example:1081", "*.internal.example;fileserver");
            WindowsProxy.apply(value);
            equal(WindowsProxy.defaultProxy(value), null, "SOCKS must not become Android HTTP ProxyInfo");
            equal(System.getProperty("http.proxyHost"), null, "SOCKS not HTTP property");
            equal(System.getProperty("socksProxyHost"), "proxy.example", "SOCKS host");
            equal(System.getProperty("socksProxyPort"), "1081", "SOCKS port");
            equal(System.getProperty("socksProxyVersion"), "5", "SOCKS version");
            for (String property : new String[] {"http.nonProxyHosts", "https.nonProxyHosts", "socksNonProxyHosts"}) {
                equal(System.getProperty(property), "*.internal.example|fileserver", property + " preserved");
            }
            // ActivityThread calls the real framework setter again while binding an app.
            android.net.Proxy.setHttpProxyConfiguration(null);
            equal(System.getProperty("http.nonProxyHosts"), "*.internal.example|fileserver",
                    "repeated null framework bind preserves HTTP SOCKS bypass");
            equal(System.getProperty("https.nonProxyHosts"), "*.internal.example|fileserver",
                    "repeated null framework bind preserves HTTPS SOCKS bypass");
            for (String scheme : new String[] {"http", "https", "socket"}) {
                equal(selected(scheme + "://public.example:80/path").type(), Proxy.Type.SOCKS,
                        scheme + " ordinary destination uses SOCKS");
                equal(selected(scheme + "://fileserver.internal.example:80/path"), Proxy.NO_PROXY,
                        scheme + " wildcard bypass");
                equal(selected(scheme + "://fileserver:80/path"), Proxy.NO_PROXY, scheme + " exact bypass");
            }
        }
        WindowsProxy.apply(config("socks=[::1]", ""));
        equal(System.getProperty("socksProxyHost"), "::1", "default-port IPv6 host");
        equal(System.getProperty("socksProxyPort"), "1080", "default-port IPv6 port");
        WindowsProxy.apply(config("socks5://[2001:db8::1]:9150", ""));
        equal(System.getProperty("socksProxyHost"), "2001:db8::1", "explicit-port IPv6 host");
        equal(System.getProperty("socksProxyPort"), "9150", "explicit-port IPv6 port");

        for (String malformed : new String[] {
                "socks=proxy:65536", "socks=proxy:-1", "socks=proxy:0", "socks=proxy:bad",
                "socks=proxy:", "socks=[::1", "socks5://user:secret@proxy:1080", "socks=proxy/path"}) {
            WindowsProxy.apply(config("socks=valid.example:1080", "*.old.example"));
            WindowsProxy.apply(config(malformed, ""));
            noSocks("malformed settings retire previous SOCKS");
            equal(selected("https://public.example/"), Proxy.NO_PROXY, "invalid proxy is not published");
        }

        WindowsProxy.apply(config("socks=proxy.example:1080;http=http-proxy.example:8080", "*.internal.example"));
        equal(selected("http://public.example/").type(), Proxy.Type.HTTP, "HTTP endpoint wins for HTTP");
        equal(selected("https://public.example/").type(), Proxy.Type.HTTP, "HTTP endpoint wins for HTTPS");
        equal(selected("socket://public.example:42").type(), Proxy.Type.SOCKS, "SOCKS retained for explicit socket selection");
        equal(WindowsProxy.serverFor("https=secure.example:443;http=plain.example:8080;socks=proxy:1080"),
                "plain.example:8080", "per-scheme HTTP priority");
        equal(WindowsProxy.serverFor("socks5://proxy:1080"), "", "SOCKS URL excluded from HTTP");
        equal(WindowsProxy.serverFor("http=socks5://proxy:1080"), "", "nested SOCKS URL excluded from HTTP");

        WindowsProxy.Config pac = config("socks=proxy.example:1080", "*.internal.example");
        pac.pacUrl = "http://pac.example/proxy.pac";
        WindowsProxy.apply(pac);
        noSocks("PAC retires static SOCKS");
        equal(WindowsProxy.defaultProxy(pac).getPacFileUrl().toString(), pac.pacUrl,
                "PAC descriptor passed to framework; evaluation is not mocked");
        check(ProxySelector.getDefault() instanceof android.net.PacProxySelector, "framework selects PAC owner");

        WindowsProxy.apply(config("socks=proxy.example:1080", "*.old.example"));
        WindowsProxy.apply(config("http-proxy.example:8080", "*.new.example"));
        noSocks("switch to HTTP retires SOCKS");
        equal(System.getProperty("http.proxyHost"), "http-proxy.example", "HTTP transition host");
        equal(System.getProperty("https.nonProxyHosts"), "*.new.example", "HTTP transition bypass");
        WindowsProxy.apply(WindowsProxy.configOf("direct"));
        for (String property : PROPERTIES) equal(System.getProperty(property), null, "direct clears " + property);
        System.out.println("PASS proxy application: bypass, endpoint rejection, IPv6, HTTP/PAC precedence and clearing");
    }

    private static Map<String, String> environment(String packageName) {
        Map<String, String> result = new HashMap<>();
        for (String item : ProxySetting.environment(packageName)) {
            int equals = item.indexOf('=');
            check(equals > 0, "well-formed child environment entry");
            result.put(item.substring(0, equals), item.substring(equals + 1));
        }
        return result;
    }

    private static void persistenceAndEnvironment() throws Exception {
        String pkg = "com.gesher.proxy.hosttest";
        String socks = ProxySetting.parse("socks=proxy.example:1080");
        ProxySetting.save(null, socks);
        equal(ProxySetting.forPackage(pkg), socks, "saved default SOCKS round-trip");
        Map<String, String> env = environment(pkg);
        equal(env.get("GESHER_PROXY"), socks, "process inherits canonical SOCKS setting");
        for (String key : new String[] {"HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"}) {
            equal(env.get(key), socks, key + " keeps SOCKS scheme");
        }
        equal(env.get("NO_PROXY"), "localhost,127.0.0.1,::1", "native client loopback exclusions");

        ProxySetting.save(pkg, "direct");
        equal(ProxySetting.forPackage(pkg), "direct", "package direct wins over global SOCKS");
        env = environment(pkg);
        for (String key : new String[] {"HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"}) {
            equal(env.get(key), "", "direct clears inherited " + key);
        }
        ProxySetting.setForLaunch(pkg, "http-proxy.example:8080");
        env = environment(pkg);
        equal(env.get("HTTP_PROXY"), "http://http-proxy.example:8080", "launch HTTP wins over saved settings");
        equal(env.get("HTTPS_PROXY"), "http://http-proxy.example:8080", "HTTP scheme preserved");
        equal(env.get("ALL_PROXY"), "", "HTTP clears inherited SOCKS ALL_PROXY");
        ProxySetting.setForLaunch(pkg, null);
        ProxySetting.save(pkg, null);
        equal(ProxySetting.forPackage(pkg), socks, "removing package override restores global SOCKS");
        ProxySetting.save(null, "windows");
        check(ProxySetting.environment(pkg).isEmpty(), "Windows mode emits no proxy overrides");
        System.out.println("PASS proxy persistence and process environment precedence");
    }

    private static void childStartup(String expected) {
        // Seed stale state to test the real startup's retirement, not merely a fresh JVM.
        System.setProperty("http.proxyHost", "stale-http.example");
        System.setProperty("socksProxyHost", "stale-socks.example");
        System.setProperty("socksProxyPort", "9999");
        equal(ProxySetting.ofProcess(), expected, "GESHER_PROXY parsed in actual child environment");
        WindowsProxy.installInProcess();
        if (expected.equals("direct")) {
            noSocks("direct startup");
            equal(System.getProperty("http.proxyHost"), null, "direct startup clears HTTP");
        } else {
            equal(WindowsProxy.config().proxy, expected, "environment override reaches config");
            equal(System.getProperty("socksProxyHost"), "127.0.0.1", "startup SOCKS host");
            equal(System.getProperty("socksProxyPort"), "1080", "startup SOCKS port");
            equal(System.getProperty("http.proxyHost"), null, "startup SOCKS not HTTP");
            equal(selected("http://localhost/"), Proxy.NO_PROXY, "startup keeps loopback direct");
        }
        System.out.println("PROXY_STARTUP_OK " + expected);
    }

    private static void startup() throws Exception {
        String java = Path.of(System.getProperty("java.home"), "bin",
                System.getProperty("os.name").startsWith("Windows") ? "java.exe" : "java").toString();
        for (String input : new String[] {"socks=127.0.0.1", "socks5=127.0.0.1:1080",
                "socks://127.0.0.1", "socks5://127.0.0.1:1080", "direct"}) {
            String expected = input.equals("direct") ? input : "socks5://127.0.0.1:1080";
            ProcessBuilder builder = new ProcessBuilder(java, "-Dgesher.test.data=" + System.getProperty("gesher.test.data"),
                    "-cp", System.getProperty("java.class.path"), ProxySettingsHostTest.class.getName(), "child", expected);
            builder.environment().put("GESHER_PROXY", input);
            builder.redirectErrorStream(true);
            Process child = builder.start();
            if (!child.waitFor(15, TimeUnit.SECONDS)) {
                child.destroyForcibly();
                child.waitFor(5, TimeUnit.SECONDS);
                throw new AssertionError("startup child timed out for " + input);
            }
            String output = new String(child.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
            equal(child.exitValue(), 0, "startup " + input + "\n" + output);
            check(output.contains("PROXY_STARTUP_OK " + expected), "startup marker " + input);
        }
        System.out.println("PASS real child JVM GESHER_PROXY startup (four SOCKS spellings and direct)");
    }

    public static void main(String[] args) throws Exception {
        if (args.length > 0 && args[0].equals("child")) { childStartup(args[1]); return; }
        Map<String, String> saved = new HashMap<>();
        for (String property : PROPERTIES) {
            saved.put(property, System.getProperty(property));
            System.clearProperty(property);
        }
        ProxySelector selector = ProxySelector.getDefault();
        try {
            parsing(); propertiesAndSelection(); persistenceAndEnvironment(); startup(); ProxySettingsRuntimeTest.main(new String[0]);
            System.out.println("PROXY_HOST_CONTRACTS_OK checks=" + checks);
        } finally {
            ProxySelector.setDefault(selector);
            for (String property : PROPERTIES) {
                String value = saved.get(property);
                if (value == null) System.clearProperty(property); else System.setProperty(property, value);
            }
        }
    }
}
