#!/usr/bin/env python3
"""Exercise production JCA and WebView reflection boundaries after actual R8.

The input classes come from the real app build. A separately compiled consumer
is deliberately kept out of R8's program inputs, as JCA / AndroidX are at run
time. Classfile output permits a host JVM check; R8ContractCheck additionally
checks the actual DEX emitted by app/build.sh. This does not launch Windows/ART.
The 0.2.0 backport excludes only the absent WindowsSpeech API; its JCA, WebView,
enclosing-class and generic-signature contracts remain exercised.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
CLASSES = ("com/gesher/app/AndroidKeyStore", "com/gesher/app/Digests",
           "com/gesher/app/webview/SupportLib",
           "org/chromium/support_lib_glue/SupportLibReflectionUtil", "android/net/IpConfiguration")

CONSUMER = r'''
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.Provider;
import java.util.HexFormat;

public final class ReflectionConsumer {
    public static void main(String[] args) throws Exception {
        int services = 0;
        for (String name : new String[] {"AndroidKeyStore", "Digests"}) {
            Provider provider = (Provider) Class.forName("com.gesher.app." + name)
                    .getConstructor().newInstance();
            for (Provider.Service service : provider.getServices()) {
                if (service.newInstance(null) == null) throw new AssertionError(service);
                services++;
            }
            if (name.equals("Digests")) {
                byte[] actual = MessageDigest.getInstance("SHA-256", provider)
                        .digest("abc".getBytes(StandardCharsets.UTF_8));
                String expected = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
                if (!HexFormat.of().formatHex(actual).equals(expected))
                    throw new AssertionError("JCA digest changed");
            }
        }
        Class<?> factory = Class.forName("com.gesher.app.webview.SupportLib$Factory");
        Object instance = factory.getConstructor().newInstance();
        String[] features = (String[]) factory.getMethod("getSupportedFeatures").invoke(instance);
        if (features.length == 0) throw new AssertionError("AndroidX features disappeared");
        for (String name : new String[] {"getStatics", "getWebkitToCompatConverter", "getProxyController"}) {
            if (factory.getMethod(name).invoke(instance) == null) throw new AssertionError(name);
        }
        Class<?> webView = Class.forName("android.webkit.WebView", false, factory.getClassLoader());
        Method create = factory.getMethod("createWebView", webView);
        if (create.getReturnType() != java.lang.reflect.InvocationHandler.class)
            throw new AssertionError("AndroidX boundary signature changed");
        Class<?> assignment = Class.forName("android.net.IpConfiguration$IpAssignment", false,
                                           factory.getClassLoader());
        Class<?> config = Class.forName("android.net.IpConfiguration", false, factory.getClassLoader());
        if (assignment.getDeclaringClass() != config)
            throw new AssertionError("boot API lost its enclosing-class metadata");
        String creator = config.getField("CREATOR").getGenericType().getTypeName();
        if (!creator.equals("android.os.Parcelable$Creator<android.net.IpConfiguration>"))
            throw new AssertionError("boot API lost its generic signature: " + creator);
        System.out.println("PASS R8 reflection: " + services + " JCA factories, SHA-256, "
                + features.length + " AndroidX features, factory methods and boot metadata");
    }
}
'''


def run(command: list[str]) -> None:
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(result.stdout)
    if "ReflectionConsumer" in command:
        print(result.stdout, end="")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-home", type=Path, required=True)
    parser.add_argument("--r8", type=Path, required=True)
    parser.add_argument("--framework-classes", type=Path, required=True)
    parser.add_argument("--app-classes", type=Path, required=True)
    parser.add_argument("--rules", type=Path, default=ROOT / "app/proguard-rules.pro")
    args = parser.parse_args()
    java = str(args.java_home / "bin/java")
    javac = str(args.java_home / "bin/javac")
    with tempfile.TemporaryDirectory(prefix="gesher-r8-reflection-") as temporary:
        work = Path(temporary)
        program = work / "program.jar"
        dependencies = work / "dependencies.jar"
        with zipfile.ZipFile(args.app_classes) as source, zipfile.ZipFile(program, "w") as target, \
                zipfile.ZipFile(dependencies, "w") as library:
            for name in source.namelist():
                selected = any(name == prefix + ".class" or name.startswith(prefix + "$")
                               for prefix in CLASSES)
                if name.endswith(".class") and selected:
                    target.writestr(name, source.read(name))
                elif name.endswith(".class"):
                    library.writestr(name, source.read(name))
        # Main's call to Digests.install() is outside this focused input, as are
        # all framework classes. Retain that real entry to model the same reachability.
        entry = work / "entry.pro"
        entry.write_text("-keep class com.gesher.app.Digests { public static void install(); }\n",
                         encoding="utf-8")
        optimized = work / "optimized.jar"
        run([java, "-Xmx768m", "-XX:ActiveProcessorCount=2", "-cp", str(args.r8),
             "com.android.tools.r8.R8", "--release", "--classfile",
             "--lib", str(args.java_home), "--lib", str(args.framework_classes),
             "--lib", str(dependencies), "--pg-conf", str(args.rules),
             "--pg-conf", str(entry), "--output", str(optimized), str(program)])
        consumer = work / "ReflectionConsumer.java"
        consumer.write_text(CONSUMER, encoding="utf-8")
        classes = work / "consumer"
        run([javac, "--release", "17", "-d", str(classes), str(consumer)])
        # The dependency jar excludes every class being tested. Using the original
        # whole app jar here would conceal removed classes via fallback loading.
        runtime = os.pathsep.join(map(str, (classes, optimized, dependencies, args.framework_classes)))
        run([java, "-cp", runtime, "ReflectionConsumer"])


if __name__ == "__main__":
    main()
