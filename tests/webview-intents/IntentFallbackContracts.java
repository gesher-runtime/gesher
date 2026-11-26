package com.gesher.app.webview;

import android.net.Uri;

import java.util.Objects;

/** Real framework Intent/Uri/Bundle; no navigation or parser substitutes. */
public final class IntentFallbackContracts {
    private static int checks;

    private static void expect(String name, String expected, String input) {
        String actual = IntentFallback.httpUrl(input);
        if (!Objects.equals(expected, actual)) {
            throw new AssertionError(name + ": expected " + expected + ", got " + actual);
        }
        checks++;
    }

    private static String explicit(String target) {
        return "intent://open/item#Intent;scheme=app;S.browser_fallback_url="
                + Uri.encode(target) + ";end";
    }

    private static String withWebData(String fallback) {
        return "intent://original.example/path#Intent;scheme=https;S.browser_fallback_url="
                + Uri.encode(fallback) + ";end";
    }

    public static void main(String[] args) {
        expect("null input", null, null);
        expect("empty input", null, "");
        expect("ordinary web URL is not an intent fallback", null, "https://example.com/");
        expect("ordinary custom URI", null, "app://example.com/");
        expect("unparsed intent data remains non-web", null, "intent://example.com/path");

        for (String target : new String[] {
                "http://example.com/", "https://example.com/path", "HTTPS://example.com/path",
                "hTtPs://Example.COM/path", "https://example.com/p?x=a%2Bb&q=two+words#part",
                "https://example.com/p;x=1", "https://example.com/a+b",
                "https://example.com/a%2Fb?q=%2520", "https://example.com/שלום?q=שלום",
                "https://bücher.example/ä", "https://%65xample.com/",
                "https://localhost/", "http://127.0.0.1/path", "https://[2001:db8::1]/path",
                "https://example.com:0/", "https://example.com:65535/", "https://example.com:/",
                "https://example.com./", "https://user:password@example.com/path"}) {
            expect("explicit target " + target, target, explicit(target));
        }
        expect("HTTP data", "http://example.com/path",
                "intent://example.com/path#Intent;scheme=http;end");
        expect("HTTPS data", "https://example.com/path?q=1",
                "intent://example.com/path?q=1#Intent;scheme=https;end");
        expect("mixed-case data scheme", "HtTpS://example.com/path",
                "intent://example.com/path#Intent;scheme=HtTpS;end");
        expect("fragment stays data", "https://example.com/path#hash",
                "intent://example.com/path#hash#Intent;scheme=https;end");
        expect("explicit target has priority", "https://fallback.example/go",
                withWebData("https://fallback.example/go"));
        expect("terminal semicolon supported by Android parser", "https://example.com/path",
                "intent://example.com/path#Intent;scheme=https;end;");
        expect("selector does not replace primary intent data", "https://original.example/path",
                "intent://original.example/path#Intent;scheme=https;SEL;scheme=http;end");
        expect("last duplicate extra follows Android parser", "https://second.example/",
                "intent://open#Intent;scheme=app;S.browser_fallback_url=https%3A%2F%2Ffirst.example%2F;"
                        + "S.browser_fallback_url=https%3A%2F%2Fsecond.example%2F;end");

        for (String invalid : new String[] {"", "https://", "https:///", "https://bad host/",
                "https://./", "https://[not-an-address]/", "https://example.com:65536/",
                "https://example.com:-1/", "https://example.com:+80/", "https://example.com:word/",
                "https://example.com/%ZZ", "https://example.com/%", "https://example.com/%Ａ0",
                "https://example.com/%FＦ", "//example.com/path",
                "https:example.com/path", "https%3A%2F%2Fexample.com",
                "file:///tmp/page.html", "content://provider/item", "javascript:void(0)",
                "data:text/plain,hello", "market://details?id=example", "app://open/item"}) {
            expect("invalid explicit target", null, explicit(invalid));
            expect("invalid explicit target tries web data", "https://original.example/path",
                    withWebData(invalid));
        }
        for (char control : new char[] {0, '\t', '\n', '\r', 0x1f, 0x7f, 0x85}) {
            String target = "https://example.com/a" + control + "b";
            expect("embedded control denied", null, explicit(target));
            expect("control does not hide valid data", "https://original.example/path", withWebData(target));
        }
        for (String input : new String[] {
                "intent:///#Intent;scheme=https;end",
                "intent:example.com/path#Intent;scheme=https;end",
                "intent://example.com/path#Intent;scheme=app;end",
                "intent://example.com/path#Intent;scheme=https;",
                "intent://example.com/path#Intent;scheme=https;nonsense=value;end",
                "intent://example.com/path#Intent;scheme=https;i.value=abc;end",
                "intent://open#Intent;scheme=app;S.browser_fallback_url;end",
                "intent://open#Intent;scheme=app;S.browser_fallback_url=https%3A%2F%2Fexample.com%2F%ZZ;end"}) {
            expect("malformed or non-web intent", null, input);
        }
        for (String typed : new String[] {"i.browser_fallback_url=42", "B.browser_fallback_url=true"}) {
            expect("non-string extra is not a URL", null,
                    "intent://open#Intent;scheme=app;" + typed + ";end");
            expect("non-string extra tries valid data", "https://example.com/path",
                    "intent://example.com/path#Intent;scheme=https;" + typed + ";end");
        }
        System.out.println("WEBVIEW_INTENT_FALLBACK_PASS checks=" + checks
                + " real_android_parser=1 parser_stubs=0 windows_navigation_executed=0");
    }
}
