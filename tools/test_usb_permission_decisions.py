#!/usr/bin/env python3
"""Run the production USB permission decision store over real host filesystem I/O.

Android UsbDevice/verified-package inputs are small immutable boundaries. The
AtomicFile host adapter uses real atomic replacement and exposes its documented
void/logged-error replacement failure, so the production durable-commit check is
exercised without replacing the identity or persistence implementation.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STUBS = {
    'android/content/pm/Signature.java': '''package android.content.pm;
public final class Signature {
    private final String value;
    public Signature(String value) { this.value = value; }
    public String toCharsString() { return value; }
}''',
    'android/hardware/usb/UsbDevice.java': '''package android.hardware.usb;
public final class UsbDevice {
    private final String name, maker, product; private final int vendor, id;
    public UsbDevice(String name, int vendor, int id, String maker, String product) {
        this.name=name; this.vendor=vendor; this.id=id; this.maker=maker; this.product=product;
    }
    public String getDeviceName() { return name; }
    public int getVendorId() { return vendor; } public int getProductId() { return id; }
    public int getDeviceClass() { return 0; } public int getDeviceSubclass() { return 0; }
    public int getDeviceProtocol() { return 0; }
    public String getManufacturerName() { return maker; } public String getProductName() { return product; }
}''',
    'com/gesher/server/ApkPackage.java': '''package com.gesher.server;
import android.content.pm.Signature;
final class ApkPackage {
    final String packageName; private final int uid; private final Signature[] signers;
    private final String owner;
    ApkPackage(String name, int uid, Signature[] signers) {
        this(name, uid, signers, "2cf7b610-cbeb-4027-a259-40e1111c1a2c");
    }
    ApkPackage(String name, int uid, Signature[] signers, String owner) {
        packageName=name; this.uid=uid; this.signers=signers; this.owner=owner;
    }
    int uid() { return uid; } Signature[] verifiedSignatures() { return signers == null ? null : signers.clone(); }
    String enabledStateOwner() { return owner; }
}''',
    'com/gesher/server/SystemServer.java': '''package com.gesher.server;
final class SystemServer { static void log(String text) { System.out.println(text); } }''',
    'android/util/AtomicFile.java': '''package android.util;
import java.io.*;
import java.nio.file.*;
public final class AtomicFile {
    public static volatile boolean failReplacement;
    private final File base, pending;
    public AtomicFile(File file) { base=file; pending=new File(file + ".new"); }
    public FileInputStream openRead() throws FileNotFoundException {
        if (base.exists() && pending.exists()) pending.delete();
        return new FileInputStream(base);
    }
    public FileOutputStream startWrite() throws IOException {
        Files.createDirectories(base.toPath().getParent());
        return new FileOutputStream(pending);
    }
    public void finishWrite(FileOutputStream output) {
        try {
            output.close();
            if (failReplacement) { failReplacement=false; return; }
            Files.move(pending.toPath(), base.toPath(), StandardCopyOption.ATOMIC_MOVE,
                       StandardCopyOption.REPLACE_EXISTING);
        } catch (IOException failure) { System.err.println("AtomicFile replacement failed: " + failure); }
    }
    public void failWrite(FileOutputStream output) {
        try { output.close(); } catch (IOException ignored) {}
        pending.delete();
    }
}''',
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    args = parser.parse_args()
    source = ROOT / 'app/java/com/gesher/server/UsbPermissionDecisions.java'
    print('USB decisions production SHA256 ' + hashlib.sha256(source.read_bytes()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix='gesher-usb-permissions-') as temporary:
        work = Path(temporary)
        classes = work / 'classes'
        classes.mkdir()
        sources = [str(source), str(ROOT / 'tests/java-performance/UsbPermissionsHost.java')]
        for relative, content in STUBS.items():
            path = work / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding='utf-8')
            sources.append(str(path))
        subprocess.run([str(args.java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-Xlint:all',
                        '-Werror', '-encoding', 'UTF-8', '-d', str(classes), *sources],
                       check=True, timeout=60)
        subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '-Xmx128m',
                        '-cp', str(classes), 'com.gesher.server.UsbPermissionsHost'],
                       check=True, timeout=60)


if __name__ == '__main__':
    main()
