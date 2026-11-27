/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.lifecycle;

import android.app.Activity;
import android.content.res.TypedArray;
import android.hardware.usb.UsbManager;
import android.os.Bundle;
import android.os.Environment;
import android.os.StatFs;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;
import java.io.File;
import java.io.FileDescriptor;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.TimeUnit;

/**
 * Reports each lifecycle callback on stdout (and in the log), and on resume what it read
 * from its resources and theme, so a test can check the manifest, the APK's resources
 * and framework-res.apk were all loaded. It sets no content view: gesher-app runs it
 * headless.
 */
public class LifecycleActivity extends Activity {
    private static final String TAG = "LifecycleActivity";

    static void report(String message) {
        System.out.println(message);
        System.out.flush();
        Log.i(TAG, message);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        report("LifecycleActivity onCreate, savedInstanceState=" + savedInstanceState);
        if (getIntent().getBooleanExtra("com.gesher.lifecycle.CHECK_USB_SERVICE", false)) {
            UsbManager usb = (UsbManager) getSystemService(USB_SERVICE);
            if (usb == null || usb.getDeviceList() == null) {
                throw new IllegalStateException("USB service unavailable in Activity.onCreate");
            }
            report("LifecycleActivity onCreate: USB service available");
        }
    }

    @Override
    protected void onStart() {
        super.onStart();
        report("LifecycleActivity onStart");
    }

    @Override
    protected void onPostCreate(Bundle savedInstanceState) {
        super.onPostCreate(savedInstanceState);
        report("LifecycleActivity onPostCreate");
    }

    @Override
    protected void onResume() {
        super.onResume();
        String marker;
        boolean actionBar;
        // In ascending order, as obtainStyledAttributes wants them.
        TypedArray a = getTheme().obtainStyledAttributes(
                new int[] {android.R.attr.windowActionBar, R.attr.gesherMarker});
        try {
            actionBar = a.getBoolean(0, true);
            marker = a.getString(1);
        } finally {
            a.recycle();
        }
        report("LifecycleActivity onResume, label=" + getTitle()
                + ", greeting=" + getString(R.string.greeting)
                + ", theme attr=" + marker
                + ", windowActionBar=" + actionBar);
        // A launch with this extra (gesher-app --ez com.gesher.lifecycle.FINISH true)
        // finishes the activity itself: the system server then pauses, stops and
        // destroys it.
        if (getIntent().getBooleanExtra("com.gesher.lifecycle.FINISH", false)) {
            report("LifecycleActivity finish()");
            finish();
        }
        // ... with com.gesher.lifecycle.REQUEST_PERMISSIONS it asks for a runtime
        // permission (the framework asks permissionmgr for its request state), and
        // finishes once the result comes.
        if (getIntent().getBooleanExtra("com.gesher.lifecycle.REQUEST_PERMISSIONS", false)) {
            requestPermissions(new String[] {android.Manifest.permission.CAMERA}, 7);
        }
        if (getIntent().getBooleanExtra("com.gesher.lifecycle.RUN_NATIVE_CHILD", false)) {
            checkNativeChildren();
        }
        if (getIntent().getBooleanExtra("com.gesher.lifecycle.CHECK_ANDROID_DATA", false)) {
            checkAndroidDataCapacity();
        }
    }

    private void checkAndroidDataCapacity() {
        StatFs filesystem = new StatFs("/data");
        if (filesystem.getBlockSizeLong() <= 0 || filesystem.getBlockCountLong() <= 0
                || filesystem.getAvailableBlocksLong() > filesystem.getBlockCountLong()) {
            throw new AssertionError("Invalid Android data filesystem capacity");
        }
        String statePath = new File(Environment.getDataDirectory(), "system/gesher-device").getPath();
        try {
            Os.close(Os.open(statePath, OsConstants.O_RDONLY, 0));
            throw new AssertionError("Installed app read the protected device state");
        } catch (ErrnoException e) {
            if (e.errno != OsConstants.EACCES && e.errno != OsConstants.EPERM) {
                throw new AssertionError("Expected the installed app's access to device state to be denied", e);
            }
        }
        report("LifecycleActivity Android data capacity and device-state isolation verified");
    }

    private String command(String... command) throws IOException, InterruptedException {
        ProcessBuilder builder = new ProcessBuilder(command)
                .redirectError(ProcessBuilder.Redirect.INHERIT);
        builder.environment().remove("GESHER_UID");
        Process child = builder.start();
        try {
            if (!child.waitFor(8, TimeUnit.SECONDS)) throw new IOException("child timed out");
            String output = new String(child.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
            if (child.exitValue() != 0) {
                throw new IOException(command[0] + " returned " + child.exitValue() + ": " + output);
            }
            return output;
        } finally {
            child.destroy();
        }
    }

    private void checkNativeChildren() {
        try {
            FileDescriptor nul = Os.open("/dev/null", OsConstants.O_RDWR | OsConstants.O_CLOEXEC, 0);
            try {
                byte[] byteValue = {42};
                if (Os.read(nul, byteValue, 0, 1) != 0 || Os.write(nul, byteValue, 0, 1) != 1
                        || !OsConstants.S_ISCHR(Os.fstat(nul).st_mode)) {
                    throw new IOException("Invalid /dev/null behavior");
                }
            } finally {
                Os.close(nul);
            }
            command("/system/bin/sleep", "2"); // Observe the actual token from the host.
            int uid = android.os.Process.myUid();
            String id = command("/system/bin/id");
            if (!id.startsWith("uid=" + uid + "(")) throw new IOException(id);
            String packages = command("/system/bin/pm", "list", "packages", getPackageName());
            if (!packages.strip().equals("package:" + getPackageName())) {
                throw new IOException(packages);
            }
            report("LifecycleActivity native child UID " + uid + " and Binder verified");
            finish();
        } catch (IOException | InterruptedException | ErrnoException failure) {
            throw new IllegalStateException("Contained native child did not complete", failure);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
            int[] grantResults) {
        StringBuilder s = new StringBuilder("LifecycleActivity onRequestPermissionsResult ")
                .append(requestCode).append(':');
        for (int i = 0; i < permissions.length; i++) {
            s.append(' ').append(permissions[i]).append('=').append(grantResults[i]);
        }
        report(s.toString());
        finish();
    }

    @Override
    protected void onPause() {
        super.onPause();
        report("LifecycleActivity onPause");
    }

    @Override
    protected void onStop() {
        super.onStop();
        report("LifecycleActivity onStop");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        report("LifecycleActivity onDestroy");
    }
}
