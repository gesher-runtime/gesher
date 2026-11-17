/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package android.content.pm;

import android.content.Context;
import android.content.Intent;
import android.content.IntentSender;
import android.os.ServiceManager;
import java.util.List;

/** Bridge to Gesher's DesktopShortcutService implementation of dynamic shortcuts. */
public final class ShortcutManager {
    private final Context mContext;
    private final IShortcutService mService;

    public ShortcutManager(Context context) {
        mContext = context;
        mService = IShortcutService.Stub.asInterface(ServiceManager.getService("shortcut"));
    }

    public boolean setDynamicShortcuts(List<ShortcutInfo> shortcutInfoList) {
        try {
            return mService.setDynamicShortcuts(mContext.getPackageName(), new ParceledListSlice<>(shortcutInfoList), 0);
        } catch (android.os.RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }

    public void addDynamicShortcuts(List<ShortcutInfo> shortcutInfoList) {
        try {
            mService.addDynamicShortcuts(mContext.getPackageName(), new ParceledListSlice<>(shortcutInfoList), 0);
        } catch (android.os.RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }

    public void removeDynamicShortcuts(List<String> shortcutIds) {
        try {
            mService.removeDynamicShortcuts(mContext.getPackageName(), shortcutIds, 0);
        } catch (android.os.RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }

    public void removeAllDynamicShortcuts() {
        try {
            mService.removeAllDynamicShortcuts(mContext.getPackageName(), 0);
        } catch (android.os.RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }

    public int getMaxShortcutCountPerActivity() {
        try {
            return mService.getMaxShortcutCountPerActivity(mContext.getPackageName(), 0);
        } catch (android.os.RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }
}
