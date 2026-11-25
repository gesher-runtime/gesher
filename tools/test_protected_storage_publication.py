#!/usr/bin/env python3
"""Run actual protected admission and policy-path/publication source on host Java.

APK verification, permission snapshot selection, native authenticated role and
Windows spawn/ACL application are explicit boundaries. Production spawn order,
environment filtering, layout selection and atomic file publication are real.
The Windows launch fixture independently checks policy ACLs and replacement.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source: str, signature: str) -> str:
    if source.count(signature) != 1:
        raise ValueError("missing or ambiguous production method: " + signature)
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


SUPPORT = r'''
package com.gesher.server;
import java.io.*;
import java.lang.reflect.*;
import java.nio.file.*;
import java.util.*;

final class PackageManagerService {}
final class ApkPackage {
    final String packageName = "fixture.protected";
    final String apkPath;
    ApkPackage(String path) { apkPath = path; }
    int uid() { return 10001; }
}
final class ApkEnrollmentVerifier {
    record Archive(String splitName, String sha256) {}
    record Identity(List<Archive> archives, List<String> currentSignerSha256) {}
    static boolean fail;
    static Identity verify(Path directory, String name) {
        if (fail) throw new SecurityException("fixture verification failure");
        return new Identity(List.of(new Archive(null, "a".repeat(64))), List.of("b".repeat(64)));
    }
}
final class AppSandbox {
    static void prepareStoragePolicyFile(String name, File file) {}
}
final class Zygote {
    static boolean protectedRole;
    static int calls;
    static boolean nativeProtectedRuntimeEnabled() { return protectedRole; }
    static String windowsPath(String value) { return value; }
    static int nativeStartProtectedRuntime(String value) { return 0; }
    static int nativeSpawnProtected(String pkg, String process, String path, String digest,
            String signer, int uid, int sdk, long sequence, String[] environment) throws IOException {
        ++calls;
        File policy = ExternalStorageAccess.policyFile(pkg);
        if (!policy.isFile()) throw new AssertionError("native admission ran before policy publication");
        String value = Files.readString(policy.toPath());
        if (!value.contains("pkg=" + pkg + "\nuid=" + uid + "\n")
                || !value.contains("mode=" + ExternalStorageAccess.mode.name() + "\n"))
            throw new AssertionError("native admission received stale or wrong-identity policy");
        for (String entry : environment) {
            if (entry.startsWith("GESHER_UID=") || entry.startsWith("GESHER_BINDER_UID=")
                    || entry.startsWith("GESHER_APP_PACKAGE=") || entry.startsWith("GESHER_BINDER="))
                throw new AssertionError("caller-selected authority identity was forwarded");
        }
        return 700 + calls;
    }
}
public final class ProtectedStoragePublicationHost {
    static int checks;
    static void check(boolean value, String message) {
        ++checks;
        if (!value) throw new AssertionError(message);
    }
    static int spawn(ApkPackage pkg) throws Exception {
        // Also run the prior production method as a semantic negative control.
        // Its old signature must still reach the native admission boundary.
        for (Method method : ProtectedRuntime.class.getDeclaredMethods()) {
            if (!method.getName().equals("spawn")) continue;
            List<String> environment = List.of("GESHER_UID=0", "GESHER_BINDER_UID=0",
                    "GESHER_APP_PACKAGE=fixture.other", "GESHER_BINDER=untrusted", "GESHER_INET=0");
            // The current signature has six parameters, the preceding one five.
            Object[] arguments = method.getParameterCount() == 6
                    ? new Object[]{new PackageManagerService(), pkg, pkg.packageName, 1L, 35, environment}
                    : new Object[]{pkg, pkg.packageName, 1L, 35, environment};
            try { return (Integer)method.invoke(null, arguments); }
            catch (InvocationTargetException error) {
                if (error.getCause() instanceof Exception cause) throw cause;
                throw (Error)error.getCause();
            }
        }
        throw new AssertionError("missing production spawn method");
    }
    public static void main(String[] args) throws Exception {
        com.gesher.app.Device.root = args[0];
        Files.createDirectories(Path.of(args[0]));
        check(ExternalStorageAccess.policyFile("fixture.protected").toPath().equals(
                Path.of(args[0], "system", "storage-policy", "fixture.protected")), "ordinary layout preserved");
        try { ExternalStorageAccess.configureProtectedRuntime(); throw new AssertionError("unprotected layout switch accepted"); }
        catch (SecurityException expected) { ++checks; }
        Zygote.protectedRole = true;
        ExternalStorageAccess.configureProtectedRuntime();
        check(ExternalStorageAccess.policyFile("fixture.protected").toPath().equals(
                Path.of(args[0], "system", "storage-policy", "fixture.protected", "policy")), "protected per-package layout");
        for (String invalid : new String[]{"../other", "fixture/other", "fixture\\other", "..", "", "fixture\u0000other"})
            check(ExternalStorageAccess.policyFile(invalid) == null, "policy path rejects invalid package");
        ApkPackage pkg = new ApkPackage(Path.of(args[0], "base.apk").toString());
        check(spawn(pkg) == 701, "first launch publishes before native admission");
        ExternalStorageAccess.mode = ExternalStorageAccess.Mode.SCOPED;
        check(spawn(pkg) == 702, "relaunch publishes current downgrade instead of reusing stale policy");
        ExternalStorageAccess.failPublication = true;
        try { spawn(pkg); throw new AssertionError("failed policy publication launched a process"); }
        catch (UncheckedIOException expected) { check(Zygote.calls == 2, "publication failure prevents native admission"); }
        ExternalStorageAccess.failPublication = false;
        ApkEnrollmentVerifier.fail = true;
        int publications = ExternalStorageAccess.publications;
        try { spawn(pkg); throw new AssertionError("unverified package launched"); }
        catch (SecurityException expected) {
            check(Zygote.calls == 2 && publications == ExternalStorageAccess.publications,
                    "failed verification precedes publication and native admission");
        }
        System.out.println("Protected storage publication: " + checks + " checks; host filesystem, explicit APK/native boundaries");
    }
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--spawn-source-ref')
    args = parser.parse_args()
    source_path = 'app/java/com/gesher/server/ProtectedRuntime.java'
    spawn = subprocess.check_output(['git', 'show', args.spawn_source_ref + ':' + source_path], cwd=ROOT).decode() \
        if args.spawn_source_ref else (ROOT / source_path).read_text()
    policy = (ROOT / 'app/java/com/gesher/server/ExternalStorageAccess.java').read_text()
    methods = '\n'.join(function(policy, signature) for signature in (
        'static void configureProtectedRuntime()', 'public static File policyFile(',
        'private static boolean isDataDirAvailable()', 'private static void writeAtomically('))
    print('production spawn SHA256 ' + hashlib.sha256(spawn.encode()).hexdigest(), flush=True)
    print('production policy methods SHA256 ' + hashlib.sha256(methods.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix='gesher-protected-policy-') as temporary:
        work = Path(temporary)
        sources = {
            'ProtectedRuntime.java': spawn,
            'ProtectedStoragePublicationHost.java': SUPPORT,
            'Device.java': 'package com.gesher.app; public class Device { public static String root; public static String dataDir() { return root; } }',
            'ExternalStorageAccess.java': '''package com.gesher.server;
import java.io.*; import java.nio.file.*; import java.nio.charset.StandardCharsets;
import com.gesher.app.Device;
final class ExternalStorageAccess {
    private static volatile boolean sProtectedPolicyDirectories;
    enum Mode { MANAGER, SCOPED }
    record Policy(Mode mode, int mediaMask) {}
    static Mode mode = Mode.MANAGER;
    static boolean failPublication;
    static int publications;
    static boolean declaresPermission(ApkPackage pkg) { return true; }
    // Permission/PM publication selection is covered by the separate production
    // storage contracts. Here the actual atomic writer runs at this boundary.
    static void syncPackage(PackageManagerService pm, ApkPackage pkg) {
        if (failPublication) throw new UncheckedIOException(new IOException("fixture write failure"));
        try { writeAtomically(pkg, new Policy(mode, 0), policyFile(pkg.packageName)); ++publications; }
        catch (IOException error) { throw new UncheckedIOException(error); }
    }
''' + methods + '\n}',
        }
        for name, content in sources.items():
            (work / name).write_text(content)
        subprocess.run([str(args.java_home / 'bin/javac'), '-d', str(work),
                        *[str(work / name) for name in sources]], check=True, timeout=60)
        subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '-cp', str(work),
                        'com.gesher.server.ProtectedStoragePublicationHost', str(work / 'data')], check=True, timeout=60)


if __name__ == '__main__':
    main()
