/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.Manifest;
import android.app.AppOpsManager;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.Build;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.UserHandle;
import android.view.WindowManager.LayoutParams;

/** System-server admission for alert windows drawn in the application process. */
public final class AlertWindowPolicy extends Binder {
    static final String NAME = "gesher.alert_window";
    private static final String DESCRIPTOR = "com.gesher.IAlertWindowPolicy";
    private static final int CHECK_ADD = IBinder.FIRST_CALL_TRANSACTION;

    /** A null package represents the system UID, which needs no client-side watcher. */
    public record Owner(int uid, String packageName) {}

    private final PackageManagerService mPm;
    private final ActivityTaskManagerService mAtm;
    private final CallerIdentity mCallers;

    AlertWindowPolicy(PackageManagerService pm, ActivityTaskManagerService atm) {
        super(DESCRIPTOR);
        mPm = pm;
        mAtm = atm;
        mCallers = new CallerIdentity(pm, atm);
    }

    public static boolean applies(LayoutParams attrs) {
        return LayoutParams.isSystemAlertWindowType(attrs.type)
                || rounded(attrs);
    }

    public static boolean rounded(LayoutParams attrs) {
        return (attrs.privateFlags & LayoutParams.PRIVATE_FLAG_IS_ROUNDED_CORNERS_OVERLAY) != 0;
    }

    /** The caller may only request admission for its own attached installation. */
    Owner checkAdd(int type, boolean rounded) {
        int pid = Binder.getCallingPid();
        int uid = Binder.getCallingUid();
        ApkPackage pkg = mAtm.authenticatedPackageCodeForPid(pid, uid);
        if (pkg == null) {
            return UserHandle.getAppId(uid) == Process.SYSTEM_UID && mCallers.isSystem()
                    ? new Owner(uid, null) : null;
        }
        if (!canAdd(type, rounded, pid, uid, pkg)
                || mAtm.authenticatedPackageCodeForPid(pid, uid) != pkg) return null;
        return new Owner(uid, pkg.packageName);
    }

    private boolean canAdd(int type, boolean rounded, int pid, int uid, ApkPackage pkg) {
        boolean internal = granted(Manifest.permission.INTERNAL_SYSTEM_WINDOW, pid, uid);
        if (rounded && !internal) return false;
        if (!LayoutParams.isSystemAlertWindowType(type)) return true;
        int target = pkg.parsed.getTargetSdkVersion();
        if (type != LayoutParams.TYPE_APPLICATION_OVERLAY && target >= Build.VERSION_CODES.O) {
            return internal;
        }
        if (granted(Manifest.permission.SYSTEM_APPLICATION_OVERLAY, pid, uid)) return true;
        int mode = mPm.appOps().noteOperation(AppOpsManager.OP_SYSTEM_ALERT_WINDOW, uid,
                pkg.packageName, null, false, "check-add", false).getOpMode();
        // Android admits ignored operations and the pre-M errored case. The session
        // owns the separate AppOps visibility decision for those windows.
        return switch (mode) {
            case AppOpsManager.MODE_ALLOWED, AppOpsManager.MODE_IGNORED -> true;
            case AppOpsManager.MODE_ERRORED -> target < Build.VERSION_CODES.M;
            default -> granted(Manifest.permission.SYSTEM_ALERT_WINDOW, pid, uid);
        };
    }

    private boolean granted(String permission, int pid, int uid) {
        return mPm.permissions().checkProcessPermission(permission, pid, uid)
                == PackageManager.PERMISSION_GRANTED;
    }

    @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
            throws RemoteException {
        if (code != CHECK_ADD) return super.onTransact(code, data, reply, flags);
        data.enforceInterface(DESCRIPTOR);
        int type = data.readInt();
        boolean rounded = data.readBoolean();
        data.enforceNoDataAvail();
        Owner owner = checkAdd(type, rounded);
        reply.writeNoException();
        reply.writeBoolean(owner != null);
        if (owner != null) {
            reply.writeInt(owner.uid());
            reply.writeString(owner.packageName());
        }
        return true;
    }

    public static Owner checkAdd(LayoutParams attrs) {
        IBinder binder = ServiceManager.checkService(NAME);
        if (binder == null) return null;
        Parcel data = Parcel.obtain();
        Parcel reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(DESCRIPTOR);
            data.writeInt(attrs.type);
            data.writeBoolean(rounded(attrs));
            if (!binder.transact(CHECK_ADD, data, reply, 0)) return null;
            reply.readException();
            return reply.readBoolean() ? new Owner(reply.readInt(), reply.readString()) : null;
        } catch (RemoteException | SecurityException denied) {
            return null;
        } finally {
            data.recycle();
            reply.recycle();
        }
    }
}
