/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ActivityManager;
import android.app.IActivityManager;
import android.app.IActivityTaskManager;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.net.Uri;
import android.os.ServiceManager;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/** The pm/am tool's Binder client. It never reads an Installer registry directly. */
public final class RuntimeCommands {
    private RuntimeCommands() {}
    private static String value(String[] args, int index) {
        if (index >= args.length) throw new IllegalArgumentException("missing option value");
        return args[index];
    }
    private static Intent intent(String[] args, int start) {
        Intent intent = new Intent();
        Uri data = null;
        String type = null;
        for (int i = start; i < args.length; ++i) {
            switch (args[i]) {
                case "-n": {
                    ComponentName component = ComponentName.unflattenFromString(value(args, ++i));
                    if (component == null) throw new IllegalArgumentException("invalid component");
                    intent.setComponent(component);
                    break;
                }
                case "-a": intent.setAction(value(args, ++i)); break;
                case "-c": intent.addCategory(value(args, ++i)); break;
                case "-p": intent.setPackage(value(args, ++i)); break;
                case "-d": data = Uri.parse(value(args, ++i)); break;
                case "-t": type = value(args, ++i); break;
                case "-f": intent.addFlags(Integer.decode(value(args, ++i))); break;
                case "--es": {
                    String name = value(args, ++i);
                    intent.putExtra(name, value(args, ++i));
                    break;
                }
                case "--ei": {
                    String name = value(args, ++i);
                    intent.putExtra(name, Integer.decode(value(args, ++i)).intValue());
                    break;
                }
                case "--ez": {
                    String name = value(args, ++i);
                    String flag = value(args, ++i);
                    if (!flag.equals("true") && !flag.equals("false")) throw new IllegalArgumentException("invalid boolean " + flag);
                    intent.putExtra(name, Boolean.parseBoolean(flag));
                    break;
                }
                case "--user": {
                    String user = value(args, ++i);
                    if (!user.equals("0") && !user.equals("current")) throw new IllegalArgumentException("only user 0 is available");
                    break;
                }
                default: throw new IllegalArgumentException("unsupported intent option " + args[i]);
            }
        }
        if (data != null || type != null) intent.setDataAndType(data, type);
        return intent;
    }
    @SuppressWarnings("unchecked")
    private static int pm(String[] args) throws Exception {
        IPackageManager service = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
        if (service == null) throw new IllegalStateException("the device's package service is unavailable");
        if (args.length == 3 && args[1].equals("path")) {
            ApplicationInfo app = service.getApplicationInfo(args[2], 0, 0);
            if (app == null) throw new IllegalArgumentException("unknown package " + args[2]);
            System.out.println("package:" + app.sourceDir);
            if (app.splitSourceDirs != null) for (String split : app.splitSourceDirs) System.out.println("package:" + split);
            return 0;
        }
        if ((args.length == 3 || args.length == 4) && args[1].equals("list") && args[2].equals("packages")) {
            String filter = args.length == 4 ? args[3] : "";
            List<PackageInfo> packages = new ArrayList<>(service.getInstalledPackages(0, 0).getList());
            packages.sort(Comparator.comparing(p -> p.packageName));
            for (PackageInfo app : packages) if (app.packageName.contains(filter)) System.out.println("package:" + app.packageName);
            return 0;
        }
        throw new IllegalArgumentException("pm list packages [FILTER] | pm path PACKAGE");
    }
    private static int am(String[] args) throws Exception {
        if (args.length < 2) throw new IllegalArgumentException("am start|force-stop|broadcast");
        if (args[1].equals("start")) {
            Intent intent = intent(args, 2).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            if (intent.getComponent() == null && intent.getAction() == null) throw new IllegalArgumentException("am start needs -n COMPONENT or -a ACTION");
            IActivityTaskManager service = IActivityTaskManager.Stub.asInterface(ServiceManager.getService("activity_task"));
            if (service == null) throw new IllegalStateException("the device's activity_task service is unavailable");
            int result = service.startActivity(null, "com.android.shell", null, intent, intent.getType(), null, null, -1, 0, null, null);
            if (result < ActivityManager.START_SUCCESS) throw new IllegalStateException("activity start failed with status " + result);
            System.out.println("Starting: " + intent);
            System.out.println("Status: ok (" + result + ")");
            return 0;
        }
        IActivityManager service = IActivityManager.Stub.asInterface(ServiceManager.getService("activity"));
        if (service == null) throw new IllegalStateException("the device's activity service is unavailable");
        if (args[1].equals("force-stop")) {
            if (args.length != 3) throw new IllegalArgumentException("am force-stop PACKAGE");
            service.forceStopPackage(args[2], 0);
            return 0;
        }
        if (args[1].equals("broadcast")) {
            Intent intent = intent(args, 2);
            if (intent.getComponent() == null && intent.getAction() == null) throw new IllegalArgumentException("am broadcast needs -n COMPONENT or -a ACTION");
            int result = service.broadcastIntentWithFeature(null, null, intent, intent.getType(), null, 0, null, null,
                    null, null, null, -1, null, false, false, 0);
            if (result != ActivityManager.BROADCAST_SUCCESS) throw new IllegalStateException("broadcast failed with status " + result);
            // The current service delivers asynchronously; no ordered result is invented.
            System.out.println("Broadcast submitted: status=" + result);
            return 0;
        }
        throw new IllegalArgumentException("am start|force-stop|broadcast");
    }
    public static void main(String[] args) {
        int result = 1;
        try {
            if (System.getenv("GESHER_BINDER") == null) throw new IllegalStateException("no Gesher device namespace");
            System.loadLibrary("android_runtime");
            if (args.length == 0) throw new IllegalArgumentException("missing tool name");
            if (args[0].equals("pm")) result = pm(args);
            else if (args[0].equals("am")) result = am(args);
            else throw new IllegalArgumentException("unknown tool " + args[0]);
        } catch (Throwable failure) {
            System.err.println((args.length == 0 ? "pm/am" : args[0]) + ": " + failure);
        }
        System.out.flush();
        System.err.flush();
        System.exit(result);
    }
}
