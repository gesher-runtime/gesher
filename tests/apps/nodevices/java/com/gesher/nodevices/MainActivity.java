/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.nodevices;

import android.app.Activity;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.location.LocationManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

/** Without ACCESS_*_LOCATION and CAMERA in its manifest: what the services answer. */
public class MainActivity extends Activity {
    private static void line(String message) {
        System.out.println("nodevices: " + message);
        System.out.flush();
        Log.i("nodevices", message);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        LocationManager lm = getSystemService(LocationManager.class);
        try {
            lm.getLastKnownLocation(LocationManager.GPS_PROVIDER);
            line("location: getLastKnownLocation answered");
        } catch (SecurityException e) {
            line("location: getLastKnownLocation throws SecurityException");
        }
        try {
            lm.requestLocationUpdates(LocationManager.NETWORK_PROVIDER, 1000, 0,
                    location -> { }, Looper.getMainLooper());
            line("location: requestLocationUpdates accepted");
        } catch (SecurityException e) {
            line("location: requestLocationUpdates throws SecurityException");
        }
        CameraManager cm = getSystemService(CameraManager.class);
        try {
            String[] ids = cm.getCameraIdList();
            line("camera: " + ids.length + " cameras");
            if (ids.length > 0) {
                cm.openCamera(ids[0], new CameraDevice.StateCallback() {
                    @Override
                    public void onOpened(CameraDevice camera) {
                        line("camera: opened");
                        camera.close();
                    }

                    @Override
                    public void onDisconnected(CameraDevice camera) {
                    }

                    @Override
                    public void onError(CameraDevice camera, int error) {
                        line("camera: onError " + error);
                    }
                }, new Handler(Looper.getMainLooper()));
                line("camera: openCamera accepted");
            }
        } catch (SecurityException e) {
            line("camera: openCamera throws SecurityException");
        } catch (Exception e) {
            line("camera: openCamera throws " + e);
        }
        line("done");
        finish();
    }
}
