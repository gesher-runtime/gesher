package com.gesher.app.webview;

import java.net.URI;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Fail-closed matching of engine-reported document URLs to listener rules. */
final class ListenerOrigins {
    private static final Pattern RULE = Pattern.compile(
            "(?i)^(https?)://(\\*\\.)?([^:/?#]+)(?::([0-9]+))?/?$");

    private ListenerOrigins() {}

    static boolean allows(String[] rules, String source) {
        if (rules == null || source == null || source.isEmpty()) return false;
        if (FileUrls.isFile(source)) {
            for (String rule : rules) if ("*".equals(rule) || "file://".equalsIgnoreCase(rule)) return true;
            return false;
        }
        final URI url;
        try { url = new URI(source); } catch (Exception ignored) { return false; }
        String scheme = url.getScheme();
        String host = url.getHost();
        if (scheme == null || host == null || url.getRawUserInfo() != null) return false;
        scheme = scheme.toLowerCase(Locale.ROOT);
        host = host.toLowerCase(Locale.ROOT);
        if (!scheme.equals("http") && !scheme.equals("https")) return false;
        int port = url.getPort();
        if (port == -1) port = scheme.equals("https") ? 443 : 80;
        if (port < 1 || port > 65535) return false;
        for (String rule : rules) {
            if ("*".equals(rule)) return true;
            if (rule == null) continue;
            Matcher m = RULE.matcher(rule);
            if (!m.matches() || !scheme.equalsIgnoreCase(m.group(1))) continue;
            String wanted = m.group(3).toLowerCase(Locale.ROOT);
            if (m.group(2) != null ? !host.endsWith("." + wanted) : !host.equals(wanted)) continue;
            int rulePort;
            try { rulePort = m.group(4) == null ? (scheme.equals("https") ? 443 : 80)
                    : Integer.parseInt(m.group(4)); }
            catch (NumberFormatException ignored) { continue; }
            if (port == rulePort) return true;
        }
        return false;
    }
}
