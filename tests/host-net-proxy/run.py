#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Compile production proxy settings and run isolated host/wire contracts.

The Android stubs below provide only unavailable framework glue. Parsing,
configuration precedence, property application, environment serialization and
startup are the production classes; TCP, SOCKS5 and HTTP use real host sockets.
This does not replace Windows/ART or WinHTTP/PAC validation.
"""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


STUBS = {
    "android/net/Uri.java": """
package android.net;
public final class Uri {
    public static final Uri EMPTY = new Uri("");
    private final String value;
    private Uri(String value) { this.value = value; }
    public static Uri parse(String value) { return new Uri(value); }
    @Override public String toString() { return value; }
    @Override public boolean equals(Object other) {
        return other instanceof Uri && value.equals(((Uri) other).value);
    }
    @Override public int hashCode() { return value.hashCode(); }
}
""",
    "android/os/IBinder.java": "package android.os; public interface IBinder {}",
    "android/os/ServiceManager.java": """
package android.os;
public final class ServiceManager {
    private static final java.util.Map<String, IBinder> sCache = new java.util.HashMap<>();
}
""",
    "com/android/net/IProxyService.java": """
package com.android.net;
public interface IProxyService {
    abstract class Stub implements IProxyService, android.os.IBinder {
        public abstract String resolvePacFile(String host, String url);
        public abstract void setPacFile(String file);
    }
}
""",
    "com/gesher/server/Device.java": """
package com.gesher.server;
public final class Device {
    public static java.io.File dataDir() {
        return new java.io.File(System.getProperty("gesher.test.data"));
    }
}
""",
    "android/net/ProxyInfo.java": """
package android.net;
public final class ProxyInfo {
    private String host;
    private int port;
    private Uri pac = Uri.EMPTY;
    private java.util.List<String> exclusions = java.util.Collections.emptyList();
    public static ProxyInfo buildPacProxy(Uri pac) {
        ProxyInfo result = new ProxyInfo(); result.pac = pac; return result;
    }
    public static ProxyInfo buildDirectProxy(String host, int port, java.util.List<String> exclusions) {
        ProxyInfo result = new ProxyInfo(); result.host = host; result.port = port;
        result.exclusions = exclusions; return result;
    }
    public String getHost() { return host; }
    public int getPort() { return port; }
    public Uri getPacFileUrl() { return pac; }
    public String[] getExclusionList() { return exclusions.toArray(new String[0]); }
}
""",
    "android/annotation/Nullable.java": "package android.annotation; public @interface Nullable {}",
    "android/annotation/SdkConstant.java": """
package android.annotation;
public @interface SdkConstant { SdkConstantType value(); enum SdkConstantType { BROADCAST_INTENT_ACTION } }
""",
    "android/annotation/SystemApi.java": """
package android.annotation;
public @interface SystemApi { Client client(); enum Client { MODULE_LIBRARIES } }
""",
    "android/compat/annotation/UnsupportedAppUsage.java": """
package android.compat.annotation;
public @interface UnsupportedAppUsage { int maxTargetSdk() default 0; long trackingBug() default 0; }
""",
    "android/content/Context.java": "package android.content; public class Context {}",
    "android/os/Build.java": """
package android.os; public class Build { public static class VERSION_CODES { public static final int R = 30; } }
""",
    "android/text/TextUtils.java": """
package android.text; public class TextUtils {
    public static boolean isEmpty(CharSequence value) { return value == null || value.length() == 0; }
}
""",
    "android/util/Log.java": """
package android.util; public class Log { public static int d(String tag, String value) { return 0; } }
""",
    "com/android/net/module/util/ProxyUtils.java": """
package com.android.net.module.util;
public class ProxyUtils {
    public static String exclusionListAsString(String[] values) {
        return values == null ? null : String.join(",", values);
    }
}
""",
    "android/net/ConnectivityManager.java": "package android.net; public class ConnectivityManager {}",
    "android/net/InetAddresses.java": """
package android.net;
public class InetAddresses {
    public static java.net.InetAddress parseNumericAddress(String value) {
        // Unused by the setter under test; do not silently provide host DNS as a substitute.
        throw new UnsupportedOperationException("numeric-address API is outside this fixture");
    }
}
""",
    "android/net/PacProxySelector.java": """
package android.net;
public class PacProxySelector extends java.net.ProxySelector {
    public java.util.List<java.net.Proxy> select(java.net.URI uri) {
        throw new UnsupportedOperationException("PAC execution requires Windows/ART integration");
    }
    public void connectFailed(java.net.URI uri, java.net.SocketAddress address, java.io.IOException error) {}
}
""",
}

# AOSP android-16.0.0_r4, frameworks/base/core/java/android/net/Proxy.java.
# The production patch is compiled against this source, not a copied setter.
PROXY_SOURCE_SHA256 = "5c96b890fa653ced45a7e4c7a93035e782e6aea62f2af71374e7f54830ac5570"


def framework_proxy(root, source, work):
    patch = root / "patches/frameworks/base/0786-proxy-socks-bypass.patch"
    tree = work / "framework"
    target = tree / "core/java/android/net/Proxy.java"
    target.parent.mkdir(parents=True)
    shutil.copyfile(source, target)
    if hashlib.sha256(target.read_bytes()).hexdigest() != PROXY_SOURCE_SHA256:
        # Framework CI has already applied repository patches. Reverse only this
        # patch on the temporary copy to establish the pinned source identity.
        subprocess.run(["git", "apply", "--reverse", str(patch)], cwd=tree, check=True,
                       capture_output=True, timeout=10)
        if hashlib.sha256(target.read_bytes()).hexdigest() != PROXY_SOURCE_SHA256:
            raise RuntimeError("Proxy.java is not the pinned Android 16r4 source, with or without patch 0786")
    subprocess.run(["git", "apply", str(patch)], cwd=tree, check=True, timeout=10)
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-home", type=Path, default=os.environ.get("JAVA_HOME"))
    parser.add_argument("--proxy-source", type=Path, help="pinned AOSP Proxy.java, original or repository-patched")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    proxy_source = args.proxy_source or root / "third_party/aosp/frameworks/base/core/java/android/net/Proxy.java"
    if not proxy_source.is_file():
        parser.error("AOSP Proxy.java is required; fetch the pinned framework source or pass --proxy-source PATH")
    suffix = ".exe" if os.name == "nt" else ""
    if args.java_home:
        java = str(args.java_home / "bin" / ("java" + suffix))
        javac = str(args.java_home / "bin" / ("javac" + suffix))
    else:
        java = shutil.which("java")
        javac = shutil.which("javac")
    if not java or not javac:
        parser.error("a JDK is required; pass --java-home or set JAVA_HOME")
    with tempfile.TemporaryDirectory(prefix="gesher-proxy-contract-") as directory:
        work = Path(directory)
        sources = []
        for name, contents in STUBS.items():
            path = work / "stubs" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents, encoding="utf-8")
            sources.append(path)
        sources.extend([
            framework_proxy(root, proxy_source, work),
            root / "app/java/com/gesher/net/ProxySetting.java",
            root / "app/java/com/gesher/net/WindowsProxy.java",
            root / "tests/dex-services/com/gesher/net/ProxySettingsRuntimeTest.java",
            Path(__file__).with_name("ProxySettingsHostTest.java"),
        ])
        classes = work / "classes"
        classes.mkdir()
        subprocess.run([javac, "--release", "11", "-encoding", "UTF-8", "-d", str(classes),
                        *map(str, sources)], check=True, timeout=90)
        environment = os.environ.copy()
        environment.pop("GESHER_PROXY", None)
        subprocess.run([java, "-Dgesher.test.data=" + str(work / "data"), "-cp", str(classes),
                        "com.gesher.net.ProxySettingsHostTest"],
                       env=environment, check=True, timeout=90)


if __name__ == "__main__":
    main()
