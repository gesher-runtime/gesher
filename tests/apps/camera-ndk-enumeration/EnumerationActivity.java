/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package org.gesher.contracts.camerandk;

import android.app.Activity;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraManager;
import android.os.Bundle;
import android.util.Log;
import java.util.Arrays;

public final class EnumerationActivity extends Activity {
    private static final String TAG = "CameraNdkEnumeration";

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        try {
            CameraManager manager = getSystemService(CameraManager.class);
            String[] javaIds = manager.getCameraIdList();
            String[] startupIds = ((ProbeApplication) getApplication()).startupIds;
            String[] nativeIds = ProbeApplication.enumerate();
            if (!Arrays.equals(javaIds, nativeIds) || !Arrays.equals(javaIds, startupIds)) {
                throw new AssertionError("Camera ID mismatch, Java=" + Arrays.toString(javaIds)
                        + ", native=" + Arrays.toString(nativeIds)
                        + ", startup=" + Arrays.toString(startupIds));
            }
            String report = "verified " + javaIds.length + " camera IDs " + Arrays.toString(javaIds);
            System.out.println(TAG + " " + report);
            Log.i(TAG, report);
        } catch (CameraAccessException failure) {
            throw new AssertionError("Cannot query Java Camera2", failure);
        } finally {
            finish();
        }
    }
}
