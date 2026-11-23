#!/usr/bin/env python3
"""Run production WebStorage dispatch with an observable bridge boundary.

This checks Java forwarding without a browser. Mandatory Windows fixtures
separately verify real storage effects through JNI and WebView2 at two origins.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VALUE_CALLBACK = """package android.webkit;
public interface ValueCallback<T> { void onReceiveValue(T value); }
"""
WEB_STORAGE = """package android.webkit;
import java.util.Map;
public class WebStorage {
    public void getOrigins(ValueCallback<Map> callback) {}
    public void getUsageForOrigin(String origin, ValueCallback<Long> callback) {}
    public void getQuotaForOrigin(String origin, ValueCallback<Long> callback) {}
    public void deleteOrigin(String origin) {}
    public void deleteAllData() {}
}
"""
BRIDGE = """package com.gesher.app.webview;
final class WebView2Host {
    static final int DATA_ALL_DOM_STORAGE=32, DATA_FILE_SYSTEMS=1, DATA_INDEXED_DB=2,
        DATA_LOCAL_STORAGE=4, DATA_WEB_SQL=8, DATA_CACHE_STORAGE=16;
    static int globalCalls, originCalls;
    static String lastOrigin;
    static boolean clearBrowsingData(int kinds, Object callback) {
        globalCalls++; return true;
    }
    static boolean clearOriginData(String origin, Object callback) {
        originCalls++; lastOrigin=origin; return true;
    }
}
"""
TEST = r'''package com.gesher.app.webview;
public final class WebStorageDispatchTest {
    private static int checks;
    private static void check(boolean value, String message) {
        checks++;
        if (!value) throw new AssertionError(message);
    }
    public static void main(String[] args) {
        GesherWebStorage storage = new GesherWebStorage();
        storage.deleteOrigin(null);
        storage.deleteOrigin("");
        check(WebView2Host.globalCalls == 0, "empty origin cleared the profile");
        check(WebView2Host.originCalls == 0, "empty origin reached native storage");
        String[] origins = { "https://first.storage.invalid", "https://second.storage.invalid:8443",
            "invalid origin", "https://quoted.invalid/\"\n" };
        for (String origin : origins) {
            int before = WebView2Host.originCalls;
            storage.deleteOrigin(origin);
            check(WebView2Host.originCalls == before + 1, "origin request was lost");
            check(origin.equals(WebView2Host.lastOrigin), "origin was rewritten");
            check(WebView2Host.globalCalls == 0, "origin deletion cleared other sites");
        }
        storage.deleteAllData();
        check(WebView2Host.globalCalls == 1, "global deletion no longer reaches profile API");
        check(WebView2Host.originCalls == origins.length, "global deletion became origin deletion");
        System.out.println("WebStorage dispatch contracts: " + checks + " checks passed");
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-home", required=True, type=Path)
    parser.add_argument("--source-ref", help="Run an old source revision as a negative control")
    args = parser.parse_args()
    relative = "app/java/com/gesher/app/webview/GesherWebStorage.java"
    source = (ROOT / relative).read_text()
    if args.source_ref:
        source = subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                cwd=ROOT, capture_output=True, text=True, check=True).stdout
    with tempfile.TemporaryDirectory(prefix="gesher-webstorage-") as temporary:
        work = Path(temporary)
        files = {
            "android/webkit/ValueCallback.java": VALUE_CALLBACK,
            "android/webkit/WebStorage.java": WEB_STORAGE,
            "com/gesher/app/webview/WebView2Host.java": BRIDGE,
            "com/gesher/app/webview/GesherWebStorage.java": source,
            "com/gesher/app/webview/WebStorageQueries.java": """package com.gesher.app.webview;
                final class WebStorageQueries { static void query(String origin, boolean quota,
                android.webkit.ValueCallback<Long> callback) { } }""",
            "com/gesher/app/webview/WebStorageDispatchTest.java": TEST,
        }
        for name, contents in files.items():
            path = work / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents)
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-d", str(work),
                        *(str(work / name) for name in files)], check=True, timeout=60)
        subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData", "-cp", str(work),
                        "com.gesher.app.webview.WebStorageDispatchTest"], check=True, timeout=15)


if __name__ == "__main__":
    main()
