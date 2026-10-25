/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devices;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.os.Bundle;
import android.provider.MediaStore;

import java.io.File;
import java.nio.file.Files;

/**
 * MediaStore.ACTION_IMAGE_CAPTURE on the Gesher device (hwsvc2, ADR 0051): what it resolves
 * to, a capture returning a thumbnail ("data"), then one written to EXTRA_OUTPUT (this
 * app's PhotoProvider); both taken by Gesher Camera's auto-capture extra.
 */
public class CaptureTestActivity extends Activity {
    private static final String AUTO = "com.gesher.camera.extra.AUTO_CAPTURE_MS";
    private static final Uri OUTPUT = Uri.parse("content://com.gesher.devices.photos/photo.jpg");

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Intent capture = new Intent(MediaStore.ACTION_IMAGE_CAPTURE);
        ComponentName c = capture.resolveActivity(getPackageManager());
        Report.line("capture: ACTION_IMAGE_CAPTURE resolves to "
                + (c != null ? c.flattenToShortString() : "nothing"));
        if (c == null) {
            finish();
            return;
        }
        startActivityForResult(new Intent(MediaStore.ACTION_IMAGE_CAPTURE).putExtra(AUTO, 1500L), 1);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == 1) {
            Bitmap thumbnail = data != null ? data.getParcelableExtra("data", Bitmap.class) : null;
            Report.line("capture: thumbnail result " + (resultCode == RESULT_OK ? "OK" : resultCode)
                    + ", data " + (thumbnail == null ? "none"
                            : thumbnail.getWidth() + "x" + thumbnail.getHeight()));
            new File(getFilesDir(), "photo.jpg").delete();
            startActivityForResult(new Intent(MediaStore.ACTION_IMAGE_CAPTURE)
                    .putExtra(MediaStore.EXTRA_OUTPUT, OUTPUT).putExtra(AUTO, 1500L)
                    .addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION), 2);
            return;
        }
        File photo = new File(getFilesDir(), "photo.jpg");
        String what;
        try {
            byte[] jpeg = Files.readAllBytes(photo.toPath());
            Bitmap b = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.length);
            what = jpeg.length + " bytes, starts FFD8 " + (jpeg.length > 2 && (jpeg[0] & 0xff) == 0xff
                    && (jpeg[1] & 0xff) == 0xd8) + ", decodes to "
                    + (b == null ? "nothing" : b.getWidth() + "x" + b.getHeight());
        } catch (Exception e) {
            what = "not read: " + e;
        }
        Report.line("capture: EXTRA_OUTPUT result " + (resultCode == RESULT_OK ? "OK" : resultCode)
                + ", the file: " + what);
        Report.line("capture: done");
        finish();
    }
}
