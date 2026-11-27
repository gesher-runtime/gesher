#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Run the desktop per-app isolation decision against host fixtures and real storage."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "app/java/com/gesher/server/SandboxOptOut.java"
HOST = """
package com.gesher.server;
import android.content.pm.Signature;
import java.io.*;
import java.util.Properties;
class Device {
    static String root;
    static String dataDir(){ return root; }
    static Properties read(File f){
        Properties p=new Properties();
        try(var in=new FileInputStream(f)){p.load(in);return p;}
        catch(IOException missing){return null;}
    }
    static void write(File f,Properties p)throws IOException{
        f.getParentFile().mkdirs();
        try(var out=new FileOutputStream(f)){p.store(out,"fixture");}
    }
}
class ApkPackage {
    final String packageName="com.example.app", apkPath;
    final int signer;
    final long installed;
    ApkPackage(String path,int signer,long installed){
        apkPath=path;this.signer=signer;this.installed=installed;
    }
    Signature[] verifiedSignatures(){
        return signer==0 ? null : new Signature[]{new Signature(signer)};
    }
    long firstInstallTime(){return installed;}
    long versionCode(){return 2;}
    int uid(){return 10001;}
}
class Installer {}
class PackageManagerService {
    ApkPackage apk=new ApkPackage("/app/a-1/base.apk",1,101);
    ApkPackage get(String name){return name.equals(apk.packageName) ? apk : null;}
    boolean isRemovalPending(String name){return false;}
}
class ActivityTaskManagerService {
    interface RunnableIo {void run()throws IOException;}
    int stops;
    boolean stopFails;
    void withStoppedProtectedInstallation(ApkPackage expected,RunnableIo apply)throws IOException{
        if (stopFails)throw new IllegalStateException("retirement failed");
        stops++;
        apply.run();
    }
}
class SystemServer {static boolean isProtectedDevice(){return true;}}
class AccountSecretStore {static void ensureLoaded(){}}
class Zygote {
    static boolean usb;
    static boolean nativeProtectedRuntimeEnabled(){return usb;}
}
public class SandboxOptOutHost {
    static boolean accept;
    static Runnable duringConsent=()->{};
    static final String NAME="com.example.app";
    static final Installer INSTALLER=new Installer();
    static final PackageManagerService PM=new PackageManagerService();
    static final ActivityTaskManagerService ATM=new ActivityTaskManagerService();
    static boolean confirm(){duringConsent.run();return accept;}
    static String change(String op)throws Exception{
        return SandboxOptOut.change(NAME,op,INSTALLER,PM,ATM);
    }
    static int checks;
    static void check(boolean ok){if (!ok)throw new AssertionError("check "+checks);checks++;}
    public static void main(String[] args)throws Exception {
        Device.root=args[0];
        ApkPackage original=PM.apk;
        check(change("status").equals("isolated"));
        check(change("unisolated").equals("cancelled") && ATM.stops==0);
        check(!SandboxOptOut.isApproved(original));
        accept=true;
        duringConsent=()->PM.apk=new ApkPackage("/app/a-2/base.apk",1,101);
        try {change("unisolated"); throw new AssertionError("stale consent");}
        catch(IllegalStateException expected){check(ATM.stops==0);}
        check(!SandboxOptOut.isApproved(PM.apk));
        duringConsent=()->{};
        PM.apk=original;
        ATM.stopFails=true;
        try {change("unisolated");throw new AssertionError("failed retirement");}
        catch(IllegalStateException expected){check(!SandboxOptOut.isApproved(original));}
        ATM.stopFails=false;
        check(change("unisolated").equals("unisolated") && ATM.stops==1);
        check(change("status").equals("unisolated"));
        check(change("unisolated").equals("unisolated") && ATM.stops==1);
        check(!SandboxOptOut.isApproved(new ApkPackage(original.apkPath,2,101)));
        check(!SandboxOptOut.isApproved(new ApkPackage(original.apkPath,1,102)));
        check(!SandboxOptOut.isApproved(new ApkPackage("/app/a-2/base.apk",1,101)));
        ATM.stopFails=true;
        try {change("isolated");throw new AssertionError("failed retirement");}
        catch(IllegalStateException expected){check(SandboxOptOut.isApproved(original));}
        ATM.stopFails=false;
        check(change("isolated").equals("isolated") && ATM.stops==2);
        check(!SandboxOptOut.isApproved(original));
        PM.apk=new ApkPackage(original.apkPath,0,101);
        try {change("unisolated");throw new AssertionError("unsigned approval");}
        catch(SecurityException expected){check(ATM.stops==2);}
        Zygote.usb=true;
        try {change("status");throw new AssertionError("USB profile accepted override");}
        catch(UnsupportedOperationException expected){check(ATM.stops==2);}
        System.out.println("Sandbox opt-out contracts: "+checks+" passed");
    }
}
"""

with tempfile.TemporaryDirectory(prefix="gesher-sandbox-opt-out-") as temporary:
    work = pathlib.Path(temporary)
    app = work / SOURCE.name
    source = SOURCE.read_text(encoding="utf-8")
    boundary = "private static native boolean nativeConfirm(String packageName);"
    assert source.count(boundary) == 1
    app.write_text(source.replace(boundary,
                   "private static boolean nativeConfirm(String name) { return SandboxOptOutHost.confirm(); }"),
                   encoding="utf-8")
    host = work / "SandboxOptOutHost.java"
    host.write_text(HOST, encoding="utf-8")
    signature = work / "Signature.java"
    signature.write_text("package android.content.pm;"
                         "public record Signature(int id) { public byte[] toByteArray(){return new byte[]{(byte)id};}}")
    process = work / "Process.java"
    process.write_text("package android.os; public class Process { public static final int FIRST_APPLICATION_UID=10000; }")
    subprocess.run(["javac", "-d", str(work), str(app), str(host), str(signature), str(process)],
                   check=True, timeout=60)
    subprocess.run(["java", "-cp", str(work), "com.gesher.server.SandboxOptOutHost", str(work)],
                   check=True, timeout=30)
