/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ActivityManager;
import android.app.AppOpsManager;
import android.app.IActivityManager;
import android.app.IActivityTaskManager;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.ServiceManager;
import android.os.UserHandle;
import com.android.internal.app.IAppOpsService;
import java.util.ArrayList;
import java.util.Arrays;
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
        if (args.length == 4 && (args[1].equals("grant") || args[1].equals("revoke"))) {
            android.permission.IPermissionManager permissions = android.permission.IPermissionManager
                    .Stub.asInterface(ServiceManager.getService("permissionmgr"));
            if (permissions == null) throw new IllegalStateException("permission service unavailable");
            if (args[1].equals("grant")) {
                permissions.grantRuntimePermission(args[2], args[3], "default:0", 0);
            } else {
                permissions.revokeRuntimePermission(args[2], args[3], "default:0", 0, "host shell");
            }
            System.out.println("Permission " + args[1] + " completed for " + args[2]);
            return 0;
        }
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
        throw new IllegalArgumentException("pm list packages [FILTER] | pm path PACKAGE"
                + " | pm grant|revoke PACKAGE PERMISSION");
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
    private static int notification(String[] args) throws Exception {
        if (args.length != 3 || (!args[1].equals("allow-listener")
                && !args[1].equals("deny-listener"))) {
            throw new IllegalArgumentException("notification allow-listener|deny-listener PACKAGE/CLASS");
        }
        ComponentName component = ComponentName.unflattenFromString(args[2]);
        if (component == null) throw new IllegalArgumentException("Invalid listener component");
        android.app.INotificationManager service = android.app.INotificationManager.Stub
                .asInterface(ServiceManager.getService("notification"));
        if (service == null) throw new IllegalStateException("Notification service unavailable");
        service.setNotificationListenerAccessGranted(component, args[1].equals("allow-listener"), true);
        System.out.println("Notification listener access updated for " + component.getPackageName());
        return 0;
    }

    private static int appops(String[] args) throws Exception {
        IPackageManager pm = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
        if (pm == null) throw new IllegalStateException("the device's package service is unavailable");
        IAppOpsService appOps = IAppOpsService.Stub.asInterface(ServiceManager.getService(Context.APP_OPS_SERVICE));
        if (appOps == null) throw new IllegalStateException("the device's appops service is unavailable");
        if (args.length == 4 && args[1].equals("get") && args[3].equals("MANAGE_EXTERNAL_STORAGE")) {
            String pkgName = args[2];
            PackageInfo pkgInfo = pm.getPackageInfo(pkgName, PackageManager.GET_PERMISSIONS, 0);
            if (pkgInfo == null || pkgInfo.applicationInfo == null) {
                throw new IllegalArgumentException("unknown package " + pkgName);
            }
            int uid = pkgInfo.applicationInfo.uid;
            boolean declared = pkgInfo.requestedPermissions != null
                    && Arrays.asList(pkgInfo.requestedPermissions).contains(
                            android.Manifest.permission.MANAGE_EXTERNAL_STORAGE);
            int rawMode = appOps.checkOperationRaw(AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE, uid, pkgName, null);
            String modeName = switch (rawMode) {
                case AppOpsManager.MODE_ALLOWED -> "allow";
                case AppOpsManager.MODE_IGNORED -> "ignore";
                case AppOpsManager.MODE_ERRORED -> "errored";
                case AppOpsManager.MODE_DEFAULT -> "default";
                default -> "errored";
            };
            boolean allowed = rawMode == AppOpsManager.MODE_ALLOWED
                    || (rawMode == AppOpsManager.MODE_DEFAULT && (UserHandle.isCore(uid)
                            || pm.checkUidPermission(android.Manifest.permission.MANAGE_EXTERNAL_STORAGE, uid)
                                    == PackageManager.PERMISSION_GRANTED));
            System.out.println("declared=" + (declared ? 1 : 0)
                    + " mode=" + modeName
                    + " allowed=" + (allowed ? 1 : 0));
            return 0;
        }
        if (args.length == 5 && args[1].equals("set") && args[3].equals("MANAGE_EXTERNAL_STORAGE")) {
            String pkgName = args[2];
            PackageInfo pkgInfo = pm.getPackageInfo(pkgName, 0, 0);
            if (pkgInfo == null || pkgInfo.applicationInfo == null) {
                throw new IllegalArgumentException("unknown package " + pkgName);
            }
            int uid = pkgInfo.applicationInfo.uid;
            int mode = switch (args[4]) {
                case "allow" -> AppOpsManager.MODE_ALLOWED;
                case "ignore" -> AppOpsManager.MODE_IGNORED;
                case "default" -> AppOpsManager.MODE_DEFAULT;
                default -> throw new IllegalArgumentException("unsupported mode " + args[4]);
            };
            appOps.setUidMode(AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE, uid, mode);
            return 0;
        }
        throw new IllegalArgumentException("appops get PACKAGE MANAGE_EXTERNAL_STORAGE"
                + " | appops set PACKAGE MANAGE_EXTERNAL_STORAGE allow|ignore|default");
    }

    public static void main(String[] args) {
        int result = 1;
        try {
            if (System.getenv("GESHER_BINDER") == null) throw new IllegalStateException("no Gesher device namespace");
            // Host control commands are not Android apps. This must precede opening
            // Binder; app processes have already opened it with their assigned UID.
            android.system.Os.setenv("GESHER_BINDER_UID", "2000", true);
            System.loadLibrary("android_runtime");
            if (args.length == 0) throw new IllegalArgumentException("missing tool name");
            if (args[0].equals("pm")) result = pm(args);
            else if (args[0].equals("am")) result = am(args);
            else if (args[0].equals("appops")) result = appops(args);
            else if (args[0].equals("notification")) result = notification(args);
            else if (args[0].equals("spell-checker")) {
                result = SpellCheckerCommands.run(java.util.Arrays.asList(args).subList(1, args.length));
            }
            else if (args[0].equals("wait-binder")) {
                if (args.length != 4) throw new IllegalArgumentException("wait-binder DEVICE PID TIMEOUT_MS");
                if (!Zygote.waitForBinder(args[1], Integer.parseInt(args[2]), Integer.parseInt(args[3]))) {
                    throw new IllegalStateException("the broker did not complete its connection handshake");
                }
                System.out.println("binder endpoint accepted a protocol connection: " + args[1]);
                result = 0;
            }
            else throw new IllegalArgumentException("unknown tool " + args[0]);
        } catch (Throwable failure) {
            System.err.println((args.length == 0 ? "pm/am" : args[0]) + ": " + failure);
        }
        System.out.flush();
        System.err.flush();
        System.exit(result);
    }
}
