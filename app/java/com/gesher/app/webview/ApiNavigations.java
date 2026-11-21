package com.gesher.app.webview;

import java.net.IDN;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.Iterator;
import java.util.Locale;

/**
 * The WebView's own navigations (loadUrl, loadData, loadDataWithBaseURL, postUrl) that
 * WebView2 has not started yet, in the order they were issued, each with its number
 * (WebView2Host hands it to the native host with the Navigate) and its URL as WebView2
 * reports it ({@link #canonical}).
 *
 * <p>Android does not ask WebViewClient.shouldOverrideUrlLoading about these loads. Two of
 * them can be issued before WebView2 raises the first one's NavigationStarting (an app
 * that calls loadDataWithBaseURL twice in a row): WebView2 then starts both, the first
 * superseded by the second, or only the second. Each NavigationStarting that is not a
 * redirect takes the oldest issued entry with its URL; the entries before it were
 * superseded before they started. A main-frame navigation that ends without being
 * superseded ends every entry that was handed to WebView2 before it started: those
 * started (or were superseded) already, so a later page navigation to the same URL is
 * the page's. Thread-safe: issued on the WebView's Looper, the rest on the WebView thread.
 */
final class ApiNavigations {
    /** More outstanding loads than this were superseded long ago. */
    static final int LIMIT = 32;

    private static final class Entry {
        final long number;
        final String url;

        Entry(long number, String url) {
            this.number = number;
            this.url = url;
        }
    }

    private final ArrayDeque<Entry> mIssued = new ArrayDeque<>();
    private long mLast;

    /** The WebView asks WebView2 to load {@code url}: the load's number (0 for none). */
    synchronized long issued(String url) {
        if (url == null) return 0;
        if (mIssued.size() == LIMIT) mIssued.removeFirst();
        Entry entry = new Entry(++mLast, canonical(url));
        mIssued.addLast(entry);
        return entry.number;
    }

    /**
     * A main-frame NavigationStarting (not a redirect) for {@code url}: whether it is one
     * of the WebView's own loads already handed to WebView2. That entry and the
     * older ones are taken; a later queued load cannot claim a page navigation.
     */
    synchronized boolean started(String url, long handed) {
        String key = canonical(url);
        if (key == null) return false;
        boolean found = false;
        for (Entry e : mIssued) {
            if (e.number <= handed && key.equals(e.url)) {
                found = true;
                break;
            }
        }
        if (!found) return false;
        while (!key.equals(mIssued.removeFirst().url)) {
            // Superseded before it started.
        }
        return true;
    }

    /** {@code url}'s load will not start (its Navigate failed). */
    synchronized void abandoned(String url) {
        String key = canonical(url);
        if (key == null) return;
        Iterator<Entry> i = mIssued.iterator();
        while (i.hasNext()) {
            if (key.equals(i.next().url)) {
                i.remove();
                return;
            }
        }
    }

    /**
     * The main frame's document moved to {@code url} without a NavigationStarting
     * (a fragment navigation, or the page's history.pushState/replaceState), after
     * WebView2 had been handed the loads up to {@code handed}. Only a fragment load the
     * WebView issued and WebView2 had already been given ends here.
     */
    synchronized void sameDocument(String url, long handed) {
        String key = canonical(url);
        if (key == null || key.indexOf('#') < 0) return;
        Iterator<Entry> i = mIssued.iterator();
        while (i.hasNext()) {
            Entry e = i.next();
            if (e.number <= handed && key.equals(e.url)) {
                i.remove();
                return;
            }
        }
    }

    /**
     * A main-frame navigation ended (it was not superseded); WebView2 had been handed the
     * loads up to {@code handed} when it started. Those have started or will not start.
     */
    synchronized void ended(long handed) {
        while (!mIssued.isEmpty() && mIssued.peekFirst().number <= handed) {
            mIssued.removeFirst();
        }
    }

    synchronized int size() {
        return mIssued.size();
    }

    /** The document {@code url} names: {@link #canonical}, without a fragment. */
    static String document(String url) {
        String key = canonical(url);
        int hash = key != null ? key.indexOf('#') : -1;
        return hash < 0 ? key : key.substring(0, hash);
    }

    /**
     * {@code url} as WebView2 reports it (NavigationStarting, WebResourceRequested): the
     * WHATWG URL serialization Chromium gives an http(s) URL. The scheme and host are
     * lower case (an international host in its ASCII form), the scheme's default port
     * is dropped, an empty path is "/", "." and ".." segments are resolved, and the
     * characters URLs do not carry as such (spaces, non-ASCII, ...) are percent-encoded
     * as UTF-8. Applied to both sides of a comparison, so a URL WebView2 reported is
     * unchanged. Other schemes keep their text, the scheme in lower case.
     */
    static String canonical(String url) {
        if (url == null) return null;
        // The URL parser drops leading and trailing C0 controls and spaces, and every
        // tab and newline.
        int from = 0;
        int to = url.length();
        while (from < to && url.charAt(from) <= ' ') from++;
        while (to > from && url.charAt(to - 1) <= ' ') to--;
        StringBuilder trimmed = new StringBuilder(to - from);
        for (int i = from; i < to; i++) {
            char ch = url.charAt(i);
            if (ch != '\t' && ch != '\n' && ch != '\r') trimmed.append(ch);
        }
        String s = trimmed.toString();
        int colon = s.indexOf(':');
        if (colon <= 0 || !scheme(s, colon)) return s;
        String scheme = s.substring(0, colon).toLowerCase(Locale.ROOT);
        String rest = s.substring(colon + 1);
        int defaultPort = scheme.equals("http") ? 80 : scheme.equals("https") ? 443 : -1;
        if (defaultPort < 0) return scheme + ":" + rest;
        rest = rest.replace('\\', '/');
        int start = 0;
        while (start < rest.length() && rest.charAt(start) == '/') start++;
        int end = start;
        while (end < rest.length() && "/?#".indexOf(rest.charAt(end)) < 0) end++;
        String authority = rest.substring(start, end);
        String tail = rest.substring(end);
        int at = authority.lastIndexOf('@');
        String user = at >= 0 ? encode(authority.substring(0, at), USERINFO, false) + "@" : "";
        String hostPort = authority.substring(at + 1);
        int portColon = hostPort.lastIndexOf(':');
        if (portColon >= 0 && hostPort.indexOf(']', portColon) >= 0) portColon = -1;
        String host = portColon >= 0 ? hostPort.substring(0, portColon) : hostPort;
        String port = portColon >= 0 ? hostPort.substring(portColon + 1) : "";
        if (!host.startsWith("[")) {
            try {
                host = IDN.toASCII(host, IDN.ALLOW_UNASSIGNED);
            } catch (IllegalArgumentException e) {
                return s;  // not a host WebView2 loads
            }
        }
        host = host.toLowerCase(Locale.ROOT);
        if (!port.isEmpty()) {
            for (int i = 0; i < port.length(); i++) {
                if (port.charAt(i) < '0' || port.charAt(i) > '9') return s;
            }
            String digits = port.replaceFirst("^0+(?=.)", "");
            if (digits.length() > 5) return s;
            port = Integer.parseInt(digits) == defaultPort ? "" : ":" + digits;
        }
        String fragment = null;
        int hash = tail.indexOf('#');
        if (hash >= 0) {
            fragment = tail.substring(hash + 1);
            tail = tail.substring(0, hash);
        }
        String query = null;
        int question = tail.indexOf('?');
        if (question >= 0) {
            query = tail.substring(question + 1);
            tail = tail.substring(0, question);
        }
        StringBuilder out = new StringBuilder(s.length() + 16);
        out.append(scheme).append("://").append(user).append(host).append(port);
        out.append(path(encode(tail, PATH, true)));
        if (query != null) out.append('?').append(encode(query, QUERY, false));
        if (fragment != null) out.append('#').append(encode(fragment, FRAGMENT, false));
        return out.toString();
    }

    private static boolean scheme(String s, int colon) {
        char first = s.charAt(0);
        if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z'))) return false;
        for (int i = 1; i < colon; i++) {
            char ch = s.charAt(i);
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
                    || ch == '+' || ch == '-' || ch == '.')) {
                return false;
            }
        }
        return true;
    }

    // The WHATWG percent-encode sets (C0 controls, DEL and non-ASCII are always encoded).
    private static final String FRAGMENT = " \"<>`";
    private static final String QUERY = " \"#<>'";
    private static final String PATH = " \"#<>?`{}";
    private static final String USERINFO = PATH + "/:;=@[\\]^|";

    /**
     * {@code value} with the characters of {@code set} percent-encoded; existing escapes
     * get upper-case digits, and in a path an escaped unreserved character is decoded.
     */
    private static String encode(String value, String set, boolean path) {
        byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
        StringBuilder out = new StringBuilder(bytes.length + 8);
        for (int i = 0; i < bytes.length; i++) {
            int b = bytes[i] & 0xff;
            if (b == '%' && i + 2 < bytes.length && hex(bytes[i + 1]) >= 0 && hex(bytes[i + 2]) >= 0) {
                int decoded = hex(bytes[i + 1]) * 16 + hex(bytes[i + 2]);
                if (path && unreserved(decoded)) {
                    out.append((char) decoded);
                } else {
                    out.append('%').append(HEX[decoded >> 4]).append(HEX[decoded & 15]);
                }
                i += 2;
            } else if (b <= 0x20 || b >= 0x7f || set.indexOf(b) >= 0) {
                out.append('%').append(HEX[b >> 4]).append(HEX[b & 15]);
            } else {
                out.append((char) b);
            }
        }
        return out.toString();
    }

    private static final char[] HEX = "0123456789ABCDEF".toCharArray();

    private static int hex(byte b) {
        if (b >= '0' && b <= '9') return b - '0';
        if (b >= 'a' && b <= 'f') return b - 'a' + 10;
        if (b >= 'A' && b <= 'F') return b - 'A' + 10;
        return -1;
    }

    private static boolean unreserved(int c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                || c == '-' || c == '.' || c == '_' || c == '~';
    }

    /** An http(s) path with its "." and ".." segments resolved; "/" when empty. */
    private static String path(String encoded) {
        if (encoded.isEmpty()) return "/";
        String[] segments = encoded.substring(1).split("/", -1);
        ArrayDeque<String> kept = new ArrayDeque<>();
        for (int i = 0; i < segments.length; i++) {
            String segment = segments[i];
            boolean last = i == segments.length - 1;
            String lower = segment.toLowerCase(Locale.ROOT);
            if (lower.equals("..") || lower.equals(".%2e") || lower.equals("%2e.")
                    || lower.equals("%2e%2e")) {
                if (!kept.isEmpty()) kept.removeLast();
                if (last) kept.addLast("");
            } else if (lower.equals(".") || lower.equals("%2e")) {
                if (last) kept.addLast("");
            } else {
                kept.addLast(segment);
            }
        }
        StringBuilder out = new StringBuilder(encoded.length());
        for (String segment : kept) out.append('/').append(segment);
        return out.length() == 0 ? "/" : out.toString();
    }
}
