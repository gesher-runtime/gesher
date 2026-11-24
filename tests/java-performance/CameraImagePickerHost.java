/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.ContentResolver;
import android.content.Intent;
import android.graphics.Bitmap;
import android.net.Uri;
import android.provider.MediaStore;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.List;

/**
 * Host contract tests for {@link CameraImageFallback}, {@link DocumentPicker} image-capture
 * fallback, and {@link WindowsCamera} image-picker device/stream.
 */
public final class CameraImagePickerHost {
    private static int checks;

    private static void check(boolean ok, String msg) {
        ++checks;
        if (!ok) throw new AssertionError(msg);
    }

    public static void main(String[] args) throws Exception {
        if (args.length < 1) {
            throw new IllegalArgumentException("Expected temp dir argument");
        }
        File tempRoot = new File(args[0]);
        Device.sTestDataDir = new File(tempRoot, "data").getPath();
        CameraImageFallback.sTestOverride = null;

        testCameraImageFallback(tempRoot);
        testDocumentPickerImageCapture(tempRoot);
        testWindowsCameraImagePicker(tempRoot);

        System.out.println("Camera image picker PASS: " + checks + " checks");
    }

    private static void testCameraImageFallback(File tempRoot) throws Exception {
        // Default off when camera.properties does not exist.
        check(!CameraImageFallback.enabled(), "CameraImageFallback must default to false");
        check(!CameraImageFallback.file().exists(), "camera.properties must not exist initially");

        // save(true) / save(false) persistence.
        CameraImageFallback.save(true);
        check(CameraImageFallback.file().isFile(), "save(true) must create camera.properties");
        check(CameraImageFallback.enabled(), "enabled() must be true after save(true)");

        // Preserve unrelated properties across save(false) / save(true).
        Files.writeString(CameraImageFallback.file().toPath(),
                "unrelated_prop=keep\nimage_picker_fallback=true\n");
        CameraImageFallback.save(false);
        check(!CameraImageFallback.enabled(), "enabled() must be false after save(false)");
        String propsText = Files.readString(CameraImageFallback.file().toPath());
        check(propsText.contains("unrelated_prop=keep"), "save(false) must preserve unrelated properties");

        // encodeJpeg: JPEG SOI passthrough.
        File jpegFile = new File(tempRoot, "sample.jpg");
        byte[] jpegBytes = new byte[]{(byte) 0xFF, (byte) 0xD8, (byte) 0xFF, (byte) 0xE0, 1, 2, 3};
        Files.write(jpegFile.toPath(), jpegBytes);
        byte[] encodedJpeg = CameraImageFallback.encodeJpeg(jpegFile);
        check(Arrays.equals(jpegBytes, encodedJpeg), "encodeJpeg must pass through JPEG bytes unchanged");

        // encodeJpeg: non-JPEG re-encoded via BitmapFactory/Bitmap.compress.
        File pngFile = new File(tempRoot, "sample.png");
        Files.write(pngFile.toPath(), new byte[]{(byte) 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A});
        byte[] reencoded = CameraImageFallback.encodeJpeg(pngFile);
        check(reencoded.length >= 2 && (reencoded[0] & 0xFF) == 0xFF && (reencoded[1] & 0xFF) == 0xD8,
                "encodeJpeg must re-encode non-JPEG files to JPEG");

        // toNv12: rejects invalid dimensions.
        boolean threwOdd = false;
        try {
            CameraImageFallback.toNv12(jpegFile, 63, 48);
        } catch (IllegalArgumentException expected) {
            threwOdd = true;
        }
        check(threwOdd, "toNv12 must reject odd width");

        boolean threwZero = false;
        try {
            CameraImageFallback.toNv12(jpegFile, 0, 48);
        } catch (IllegalArgumentException expected) {
            threwZero = true;
        }
        check(threwZero, "toNv12 must reject zero width");

        // toNv12: produces width * height * 3 / 2 bytes.
        byte[] nv12 = CameraImageFallback.toNv12(jpegFile, 64, 48);
        check(nv12.length == 64 * 48 * 3 / 2, "toNv12 must produce width*height*3/2 bytes");
        boolean anyNonZero = false;
        for (byte b : nv12) {
            if (b != 0) {
                anyNonZero = true;
                break;
            }
        }
        check(anyNonZero, "toNv12 output must contain image pixel data");
    }

    private static void testDocumentPickerImageCapture(File tempRoot) throws Exception {
        Intent capture = new Intent(MediaStore.ACTION_IMAGE_CAPTURE);
        Intent secureCapture = new Intent(MediaStore.ACTION_IMAGE_CAPTURE_SECURE);

        // Disabled by default.
        CameraImageFallback.sTestOverride = Boolean.FALSE;
        check(!DocumentPicker.handles(capture), "ACTION_IMAGE_CAPTURE must not be handled when disabled");
        check(!DocumentPicker.resolves(capture, null), "ACTION_IMAGE_CAPTURE must not resolve when disabled");
        check(!DocumentPicker.offers(capture), "ACTION_IMAGE_CAPTURE must not be offered when disabled");
        check(!DocumentPicker.handles(secureCapture), "ACTION_IMAGE_CAPTURE_SECURE must not be handled when disabled");

        // Enabled.
        CameraImageFallback.sTestOverride = Boolean.TRUE;
        check(DocumentPicker.handles(capture), "ACTION_IMAGE_CAPTURE must be handled when enabled");
        check(DocumentPicker.resolves(capture, null), "ACTION_IMAGE_CAPTURE must resolve when enabled");
        check(DocumentPicker.offers(capture), "ACTION_IMAGE_CAPTURE must be offered when enabled");
        check(DocumentPicker.handles(secureCapture), "ACTION_IMAGE_CAPTURE_SECURE must be handled when enabled");

        File chosenImage = new File(tempRoot, "picked.jpg");
        byte[] chosenBytes = new byte[]{(byte) 0xFF, (byte) 0xD8, (byte) 0xFF, (byte) 0xD9};
        Files.write(chosenImage.toPath(), chosenBytes);
        DocumentsStorageProvider provider = new DocumentsStorageProvider();
        DocumentPicker.Backend backend = new DocumentPicker.Backend() {
            @Override public boolean available() { return true; }
            @Override public String[] pick(DocumentPicker.Request request, int ownerPid,
                    java.util.function.BooleanSupplier active) {
                return new String[]{chosenImage.getPath()};
            }
            @Override public boolean isLocal(String windowsPath) { return true; }
        };

        // Case 1: EXTRA_OUTPUT with file:// URI.
        File outFile = new File(tempRoot, "capture-out.jpg");
        Intent extraOutIntent = new Intent(MediaStore.ACTION_IMAGE_CAPTURE)
                .putExtra(MediaStore.EXTRA_OUTPUT, Uri.fromFile(outFile));
        DocumentPicker.Request extraOutReq = new DocumentPicker.Request(extraOutIntent, null, 36);
        check(extraOutReq.imageCapture, "Request.imageCapture must be true");
        check(!extraOutReq.multiple && extraOutReq.maxItems == 1, "Image capture must be single-item");
        DocumentPicker.Result extraOutRes = DocumentPicker.select(
                extraOutReq, new String[]{chosenImage.getPath()}, provider, backend, null);
        check(extraOutRes != null, "EXTRA_OUTPUT select must return a Result");
        check(Uri.fromFile(outFile).equals(extraOutRes.intent.getData()),
                "EXTRA_OUTPUT result intent data must match EXTRA_OUTPUT URI");
        check(Arrays.equals(chosenBytes, Files.readAllBytes(outFile.toPath())),
                "EXTRA_OUTPUT file must receive JPEG bytes");

        // Case 2: EXTRA_OUTPUT with content:// URI via ContentResolver.
        Uri contentOutUri = Uri.parse("content://test.authority/capture.jpg");
        ByteArrayOutputStream contentOutStream = new ByteArrayOutputStream();
        ContentResolver resolver = new ContentResolver() {
            @Override public OutputStream openOutputStream(Uri uri) {
                return contentOutUri.equals(uri) ? contentOutStream : null;
            }
        };
        Intent contentOutIntent = new Intent(MediaStore.ACTION_IMAGE_CAPTURE)
                .putExtra(MediaStore.EXTRA_OUTPUT, contentOutUri);
        DocumentPicker.Request contentOutReq = new DocumentPicker.Request(contentOutIntent, null, 36);
        DocumentPicker.Result contentOutRes = DocumentPicker.select(
                contentOutReq, new String[]{chosenImage.getPath()}, provider, backend, resolver);
        check(contentOutRes != null && contentOutUri.equals(contentOutRes.intent.getData()),
                "ContentResolver EXTRA_OUTPUT result must match URI");
        check(Arrays.equals(chosenBytes, contentOutStream.toByteArray()),
                "ContentResolver EXTRA_OUTPUT stream must receive JPEG bytes");

        // Case 3: No EXTRA_OUTPUT -> inline "data" thumbnail Bitmap.
        DocumentPicker.Request inlineReq = new DocumentPicker.Request(capture, null, 36);
        DocumentPicker.Result inlineRes = DocumentPicker.select(
                inlineReq, new String[]{chosenImage.getPath()}, provider, backend, null);
        check(inlineRes != null && "inline-data".equals(inlineRes.intent.getAction()),
                "Inline capture must return inline-data action");
        Object thumbObj = inlineRes.intent.getExtra("data");
        check(thumbObj instanceof Bitmap, "Inline capture must include Bitmap under 'data' extra");
        Bitmap thumb = (Bitmap) thumbObj;
        check(thumb.getWidth() == 160, "Inline thumbnail must be scaled to width 160");

        CameraImageFallback.sTestOverride = null;
    }

    private static void testWindowsCameraImagePicker(File tempRoot) throws Exception {
        // IMAGE_PICKER_ID only present when CameraImageFallback is enabled.
        CameraImageFallback.sTestOverride = Boolean.FALSE;
        List<WindowsCamera.Device> offDevices = WindowsCamera.devices();
        for (WindowsCamera.Device d : offDevices) {
            check(!WindowsCamera.IMAGE_PICKER_ID.equals(d.id),
                    "IMAGE_PICKER_ID must not be present when disabled");
        }

        CameraImageFallback.sTestOverride = Boolean.TRUE;
        List<WindowsCamera.Device> onDevices = WindowsCamera.devices();
        WindowsCamera.Device pickerDevice = null;
        for (WindowsCamera.Device d : onDevices) {
            if (WindowsCamera.IMAGE_PICKER_ID.equals(d.id)) {
                pickerDevice = d;
                break;
            }
        }
        check(pickerDevice != null, "IMAGE_PICKER_ID must be present when enabled");
        check("Gesher Image Picker".equals(pickerDevice.name), "Unexpected picker device name");
        check(pickerDevice.isRearFacing() && !pickerDevice.autofocus,
                "Picker device must be rear-facing and fixed-focus");

        // open(IMAGE_PICKER_ID) with injected ImageSource -> NV12 frames.
        File streamImage = new File(tempRoot, "stream.jpg");
        Files.write(streamImage.toPath(), new byte[]{(byte) 0xFF, (byte) 0xD8, (byte) 0xFF, (byte) 0xD9});
        WindowsCamera.sImageSource = () -> new String[]{streamImage.getPath()};
        try (WindowsCamera.Capture capture = WindowsCamera.open(WindowsCamera.IMAGE_PICKER_ID, 64, 48)) {
            check(capture.width == 64 && capture.height == 48, "Capture dimensions must match");
            byte[] frame = new byte[64 * 48 * 3 / 2];
            int hr = capture.read(frame);
            check(hr == 0, "Capture.read must return 0 while open");
            check(capture.focus(WindowsCamera.Capture.FOCUS_GET, null) == 0x80004001,
                    "Static image capture focus must return E_NOTIMPL");
            capture.close();
            check(capture.read(frame) == 0x80004004, "Capture.read after close must return E_ABORT");
            check(capture.focus(WindowsCamera.Capture.FOCUS_GET, null) == 0x80004004,
                    "Capture.focus after close must return E_ABORT");
        } finally {
            WindowsCamera.sImageSource = null;
        }

        // open(IMAGE_PICKER_ID) cancelled when ImageSource returns null.
        WindowsCamera.sImageSource = () -> null;
        boolean cancelled = false;
        try {
            WindowsCamera.open(WindowsCamera.IMAGE_PICKER_ID, 64, 48);
        } catch (WindowsCamera.OpenException expected) {
            cancelled = expected.hr == 0x800704C7;
        } finally {
            WindowsCamera.sImageSource = null;
            CameraImageFallback.sTestOverride = null;
        }
        check(cancelled, "Cancelled image pick must throw OpenException with ERROR_CANCELLED (0x800704C7)");
    }
}
