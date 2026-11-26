package com.gesher.app.webview;

import android.content.Intent;
import android.net.Uri;

import java.net.IDN;
import java.net.URL;

/** Selects a usable web target from an otherwise unhandled Android intent URI. */
final class IntentFallback {
    private IntentFallback() { }

    static String httpUrl(String url) {
        if (url == null || !url.startsWith("intent:")) return null;
        try {
            Intent intent = Intent.parseUri(url, Intent.URI_INTENT_SCHEME);
            // A non-string extra is not a URL. Inspect its type without asking
            // Bundle's string accessor to log a malformed application's value.
            android.os.Bundle extras = intent.getExtras();
            Object extra = extras == null ? null : extras.get("browser_fallback_url");
            String fallback = extra instanceof String ? webUrl((String) extra) : null;
            return fallback != null ? fallback : webUrl(intent.getDataString());
        } catch (Exception invalid) {
            return null;
        }
    }

    private static String webUrl(String value) {
        if (value == null || !(value.regionMatches(true, 0, "https://", 0, 8)
                || value.regionMatches(true, 0, "http://", 0, 7))) return null;
        for (int i = 0; i < value.length(); i++) {
            char ch = value.charAt(i);
            if (Character.isISOControl(ch)) return null;
            if (ch == '%' && (i + 2 >= value.length()
                    || !hex(value.charAt(i + 1)) || !hex(value.charAt(i + 2)))) return null;
        }
        try {
            // URL parses the authority/port without opening a connection. Uri
            // supplies the decoded host, including ordinary encoded IDN names.
            URL parsed = new URL(value);
            String host = Uri.parse(value).getHost();
            if (host == null || host.isEmpty()) return null;
            if (!host.startsWith("[")) {
                String ascii = IDN.toASCII(host, IDN.USE_STD3_ASCII_RULES);
                if (ascii.isEmpty() || ascii.equals(".")) return null;
            }
            // URL accepts an explicit -1 port as its "unspecified" sentinel;
            // a web authority may instead omit the port or contain digits.
            String authority = parsed.getAuthority();
            String server = authority.substring(authority.lastIndexOf('@') + 1);
            int colon = server.lastIndexOf(':');
            if (colon > server.lastIndexOf(']')) {
                for (int i = colon + 1; i < server.length(); i++) {
                    char ch = server.charAt(i);
                    if (ch < '0' || ch > '9') return null;
                }
            }
            if (parsed.getPort() > 65535) return null;
            return value;
        } catch (Exception invalid) {
            return null;
        }
    }

    private static boolean hex(char ch) {
        return ch >= '0' && ch <= '9' || ch >= 'a' && ch <= 'f' || ch >= 'A' && ch <= 'F';
    }
}
