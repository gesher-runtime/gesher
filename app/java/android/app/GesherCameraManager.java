/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package android.app;

import android.content.Context;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraManager;

/** The app's ordinary Camera2 discovery path, reached from libcamera2ndk. */
final class GesherCameraManager {
    private GesherCameraManager() { }

    private static String[] getCameraIdList() throws CameraAccessException {
        ActivityThread thread = ActivityThread.currentActivityThread();
        if (thread == null) return null;
        Context context = thread.getApplication();
        if (context == null && thread.mBoundApplication != null
                && thread.mBoundApplication.info != null) {
            // A library can query during Application construction, before the
            // Application is published. Use the bound APK's attribution there.
            context = ContextImpl.createAppContext(thread, thread.mBoundApplication.info);
        }
        if (context == null) return null;
        CameraManager manager = context.getSystemService(CameraManager.class);
        return manager == null ? null : manager.getCameraIdList();
    }
}
