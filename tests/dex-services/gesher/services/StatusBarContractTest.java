/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.app.StatusBarManager;
import android.content.ComponentName;
import android.content.res.AssetManager;
import android.graphics.drawable.Icon;
import android.os.IBinder;

import com.android.internal.statusbar.IAddTileResultCallback;
import com.android.internal.statusbar.IStatusBarService;

import java.util.Collections;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * {@code statusbar} with no SystemUI status bar: the service exists (StatusBarManager
 * dereferenced null without it), and the public tile APIs answer as
 * StatusBarManagerService does while no bar is registered.
 */
public final class StatusBarContractTest {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        Map<String, IBinder> services =
                com.gesher.server.Services.createInProcess(Collections.emptyList());
        IBinder binder = services.get("statusbar");
        require(binder != null, "no statusbar service");
        IStatusBarService statusBar = IStatusBarService.Stub.asInterface(binder);
        require(statusBar != null, "statusbar is not an IStatusBarService");

        ComponentName tile = new ComponentName("gesher.missing", "gesher.missing.Tile");
        // A listening request has no status bar to reach: it returns.
        statusBar.requestTileServiceListeningState(tile, 0);

        AtomicInteger result = new AtomicInteger(Integer.MIN_VALUE);
        IAddTileResultCallback callback = new IAddTileResultCallback.Stub() {
            @Override public void onTileRequest(int code) { result.set(code); }
        };
        Icon icon = Icon.createWithBitmap(android.graphics.Bitmap.createBitmap(1, 1,
                android.graphics.Bitmap.Config.ARGB_8888));
        statusBar.requestAddTile(tile, "Tile", icon, 10, callback);
        require(result.get() == StatusBarManager.TILE_ADD_REQUEST_ERROR_NOT_CURRENT_USER,
                "another user's request answered " + result.get());
        result.set(Integer.MIN_VALUE);
        statusBar.requestAddTile(tile, "Tile", icon, 0, callback);
        require(result.get() == StatusBarManager.TILE_ADD_REQUEST_ERROR_BAD_COMPONENT,
                "a component that is no tile service answered " + result.get());
        statusBar.cancelRequestAddTile("gesher.missing");
        System.out.println("StatusBarContractTest all passed");
    }
}
