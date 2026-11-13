#!/usr/bin/env python3
"""Exercise production message ports and provider transfer methods.

The public fixture compiles against the official SDK. Host execution replaces
only Handler/Looper scheduling and the WebView2 host with declared adapters;
the provider's unrelated destroy dependencies are no-op boundary adapters.
WebMessage, payload encoding, port ownership and provider methods are real.
The same public fixture runs with real Handler/Looper in the Windows ART gate.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ADAPTERS = {
    "android/os/Looper.java": '''package android.os;
public final class Looper {
    final java.util.concurrent.LinkedBlockingQueue<Runnable> tasks = new java.util.concurrent.LinkedBlockingQueue<>();
    static final Runnable STOP = () -> {};
    volatile boolean quit;
    public static Looper getMainLooper() { throw new AssertionError("Fixture must supply a Handler"); }
}''',
    "android/os/Handler.java": '''package android.os;
public class Handler {
    private final Looper looper;
    public Handler(Looper looper) { this.looper = looper; }
    public boolean post(Runnable work) { if (looper.quit) return false; return looper.tasks.offer(work); }
}''',
    "android/os/HandlerThread.java": '''package android.os;
public class HandlerThread extends Thread {
    private final Looper looper = new Looper();
    public HandlerThread(String name) { super(name); }
    public Looper getLooper() { return looper; }
    public boolean quitSafely() { looper.quit = true; looper.tasks.offer(Looper.STOP); return true; }
    public void run() {
        try { for (;;) { Runnable work = looper.tasks.take(); if (work == Looper.STOP) return; work.run(); } }
        catch (InterruptedException e) { interrupt(); }
    }
}''',
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-home", required=True, type=Path)
    parser.add_argument("--framework-classes", required=True, type=Path)
    parser.add_argument("--core-classes", required=True, type=Path)
    parser.add_argument("--sdk-jar", required=True, type=Path)
    parser.add_argument("--source-ref")
    parser.add_argument("--case", choices=("all", "public", "page", "ownership"), default="all")
    args = parser.parse_args()
    java, javac = (str(args.java_home / "bin" / name) for name in ("java", "javac"))
    classpath = os.pathsep.join(str(p.resolve()) for p in (args.framework_classes, args.core_classes))

    def source(path: str) -> str:
        if args.source_ref:
            return subprocess.run(["git", "show", args.source_ref + ":" + path], cwd=ROOT,
                                  check=True, capture_output=True, text=True).stdout
        return (ROOT / path).read_text()

    with tempfile.TemporaryDirectory(prefix="gesher-webmessage-") as temporary:
        work = Path(temporary)
        classes = work / "classes"; classes.mkdir()
        public = work / "public"; public.mkdir()
        subprocess.run([javac, "-encoding", "UTF-8", "-cp", str(args.sdk_jar), "-d", str(public),
                        str(ROOT / "tests/dex-services/gesher/services/WebMessagePortContractTest.java")], check=True)
        inputs = []
        for relative, content in ADAPTERS.items():
            path = work / relative; path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content); inputs.append(str(path))
        package = work / "com/gesher/app/webview"; package.mkdir(parents=True)
        for name in ("GesherWebMessagePort.java", "WebMessagePayload.java"):
            path = package / name; path.write_text(source("app/java/com/gesher/app/webview/" + name)); inputs.append(str(path))
        provider = source("app/java/com/gesher/app/webview/GesherWebViewProvider.java")
        start = provider.index("    public void postMessageToMainFrame(")
        end = provider.index("    @Override\n    public WebSettings getSettings()", start)
        methods = provider[start:end]
        destroy_start = provider.index("    public void destroy() {")
        destroy_end = provider.index("    @Override\n    public void setNetworkAvailable(", destroy_start)
        methods += provider[destroy_start:destroy_end]
        adapter = '''package com.gesher.app.webview;
import android.webkit.WebMessage;
import android.webkit.WebMessagePort;
import android.net.Uri;
import org.json.JSONObject;
import org.json.JSONArray;
final class WebView2Host {
    final java.util.List<String> sent = new java.util.ArrayList<>();
    java.util.function.Consumer<String> afterSend;
    boolean destroyed;
    void postWebMessage(String message) {
        sent.add(message);
        if (afterSend != null) afterSend.accept(message);
    }
    void destroy() { destroyed = true; }
}
final class CleanupBoundary { void close() {} void pageGone() {} }
final class FileUrls { static String toWebView2(String url) { return url; } }
final class GesherWebViewProvider {
    final java.util.Map<String,GesherWebMessagePort> mPagePorts = new java.util.LinkedHashMap<>();
    volatile WebView2Host mHost = new WebView2Host();
    boolean mDestroyed;
    final CleanupBoundary mBridgeDispatcher = new CleanupBoundary();
    final CleanupBoundary mGeolocation = new CleanupBoundary();
    void removeListeners() {}
''' + methods + "\n}\n"
        path = package / "GesherWebViewProvider.java"; path.write_text(adapter); inputs.append(str(path))
        inputs.append(str(ROOT / "tests/webmessage/PagePortBoundaryTest.java"))
        inputs.append(str(ROOT / "tests/webmessage/UndeliveredPortOwnershipTest.java"))
        subprocess.run([javac, "-encoding", "UTF-8", "-cp", classpath, "-d", str(classes), *inputs], check=True)
        runtime = os.pathsep.join((str(public), str(classes), classpath))
        if args.case in ("all", "public"):
            subprocess.run([java, "-cp", runtime, "gesher.services.WebMessagePortContractTest", "--host"], check=True, timeout=45)
        if args.case in ("all", "page"):
            subprocess.run([java, "-cp", runtime, "com.gesher.app.webview.PagePortBoundaryTest"], check=True, timeout=30)
        if args.case in ("all", "ownership"):
            subprocess.run([java, "-cp", runtime, "com.gesher.app.webview.UndeliveredPortOwnershipTest"], check=True, timeout=30)


if __name__ == "__main__":
    main()
