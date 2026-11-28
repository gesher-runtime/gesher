/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.os.ServiceManager;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.function.BooleanSupplier;

/**
 * Production CameraImageFallback, CameraImageSource, CameraSourceService (its answer and its
 * Binder round trip) and WindowsCamera's selected-image capture, over
 * tools/test_camera_image_picker.py's shims of the framework and of the server's Windows dialogs.
 */
public final class CameraImagePickerHost {
    private static int sChecks;

    private static void check(boolean ok, String message) {
        if (!ok) throw new AssertionError(message);
        sChecks++;
    }

    public static void main(String[] args) throws Exception {
        File temp = new File(args[0]);
        setting(temp);
        answers(temp);
        binderRoundTrip(temp);
        clientChoices();
        frames();
        System.out.println("Camera image picker PASS: " + sChecks + " checks");
    }

    private static void setting(File temp) throws IOException {
        File data = new File(temp, "data");
        CameraImageFallback.sDataDirForTest = data.getPath();
        check(!CameraImageFallback.enabled(), "off without camera.properties");
        File props = new File(data, "system/camera.properties");
        props.getParentFile().mkdirs();
        Files.write(props.toPath(), "#Gesher camera settings\nimage_picker_fallback=true\n"
                .getBytes(StandardCharsets.UTF_8));
        check(CameraImageFallback.enabled(), "on with image_picker_fallback=true");
        Files.write(props.toPath(), "image_picker_fallback : TRUE\n".getBytes(StandardCharsets.UTF_8));
        check(CameraImageFallback.enabled(), "Properties syntax (colon, case) is accepted");
        Files.write(props.toPath(), "image_picker_fallback=false\n".getBytes(StandardCharsets.UTF_8));
        check(!CameraImageFallback.enabled(), "off with image_picker_fallback=false");
        CameraImageFallback.sDataDirForTest = null;
    }

    /** A scripted Windows question and file picker. */
    static final class FakeUi implements CameraSourceService.Ui {
        boolean available = true;
        int answer;
        String path;
        final List<String> calls = new ArrayList<>();

        @Override public boolean available() { return available; }

        @Override public int ask(String requester, String title, String[] labels,
                BooleanSupplier active) {
            calls.add("ask:" + requester + ":" + labels.length);
            return answer;
        }

        @Override public String pick(String title, int ownerPid, BooleanSupplier active) {
            calls.add("pick:" + ownerPid);
            if (whilePicking != null) whilePicking.run();
            return path;
        }

        Runnable whilePicking;
        boolean refuse;
        CameraImageSource.Framing framing = CameraImageSource.Framing.DEFAULT;
        boolean cancelFraming;

        @Override public File resolve(String windowsPath) throws IOException {
            if (refuse) throw new IOException("Android data/obb is not selectable");
            return new File(windowsPath);
        }

        @Override public CameraImageSource.Framing frame(String title, File file, int ownerPid,
                BooleanSupplier active) {
            calls.add("frame:" + ownerPid);
            return cancelFraming ? null : framing;
        }
    }

    static final class FakeCallers implements CameraSourceService.Callers {
        boolean granted = true;
        boolean attached = true;

        @Override public String packageOf(int pid, int uid) {
            if (pid == 666) throw new SecurityException("unknown caller");
            return "com.example.bank";
        }

        @Override public boolean cameraGranted(int pid, int uid) { return granted; }

        @Override public boolean attached(int pid) { return attached; }
    }

    private static void answers(File temp) throws IOException {
        File image = new File(temp, "cheque.jpg");
        Files.write(image.toPath(), new byte[] {(byte) 0xFF, (byte) 0xD8, (byte) 0xFF, 1, 2, 3});
        FakeUi ui = new FakeUi();
        FakeCallers callers = new FakeCallers();
        boolean[] enabled = {false};
        CameraSourceService service = new CameraSourceService(callers, ui, () -> enabled[0]);

        CameraSourceService.Answer a = service.answer(10, 10010, "0", true);
        check(a.kind == CameraImageSource.Selection.CAMERA && ui.calls.isEmpty(),
                "setting off: a PC camera opens without a question");
        a = service.answer(10, 10010, CameraImageSource.DEVICE_ID, false);
        check(a.kind == CameraImageSource.Selection.CANCELLED && ui.calls.isEmpty(),
                "setting off: no selected-image camera");

        enabled[0] = true;
        boolean refused = false;
        try {
            service.answer(666, 10666, "0", true);
        } catch (SecurityException expected) {
            refused = true;
        }
        check(refused && ui.calls.isEmpty(), "an unknown caller is refused before any dialog");
        callers.granted = false;
        refused = false;
        try {
            service.answer(10, 10010, "0", true);
        } catch (SecurityException expected) {
            refused = true;
        }
        check(refused && ui.calls.isEmpty(), "without the CAMERA grant there is no question");
        callers.granted = true;

        ui.available = false;
        a = service.answer(10, 10010, "0", true);
        check(a.kind == CameraImageSource.Selection.CAMERA && ui.calls.isEmpty(),
                "no dialog on this desktop: the PC camera");
        ui.available = true;

        ui.answer = IntentChooser.UNAVAILABLE;
        a = service.answer(10, 10010, "0", true);
        check(a.kind == CameraImageSource.Selection.CAMERA, "an unavailable dialog: the PC camera");
        ui.calls.clear();
        ui.answer = 0;
        a = service.answer(10, 10010, "0", true);
        check(a.kind == CameraImageSource.Selection.CAMERA
                && ui.calls.equals(List.of("ask:com.example.bank:2")),
                "the user chose the camera: asked with the app's package, no file picker");
        ui.calls.clear();
        ui.answer = -1;
        a = service.answer(10, 10010, "0", true);
        check(a.kind == CameraImageSource.Selection.CANCELLED && ui.calls.size() == 1,
                "a cancelled question cancels without a file picker");

        ui.calls.clear();
        ui.answer = 1;
        ui.path = image.getPath();
        a = service.answer(10, 10010, "0", true);
        check(a.kind == CameraImageSource.Selection.IMAGE && image.equals(a.image)
                && ui.calls.equals(List.of("ask:com.example.bank:2", "pick:10", "frame:10")),
                "the user chose an image: the file picker and framing preview, owned by the app's process");

        ui.calls.clear();
        a = service.answer(10, 10010, CameraImageSource.DEVICE_ID, false);
        check(a.kind == CameraImageSource.Selection.IMAGE
                && ui.calls.equals(List.of("pick:10", "frame:10")),
                "the selected-image camera goes straight to the file picker and framing preview");

        ui.cancelFraming = true;
        a = service.answer(10, 10010, CameraImageSource.DEVICE_ID, false);
        check(a.kind == CameraImageSource.Selection.CANCELLED,
                "cancelling the framing preview cancels the selection");
        ui.cancelFraming = false;

        ui.path = null;
        a = service.answer(10, 10010, CameraImageSource.DEVICE_ID, false);
        check(a.kind == CameraImageSource.Selection.CANCELLED, "a cancelled file picker cancels");
        ui.path = new File(temp, "missing.jpg").getPath();
        a = service.answer(10, 10010, CameraImageSource.DEVICE_ID, false);
        check(a.kind == CameraImageSource.Selection.CANCELLED, "a missing file is not an image");
        ui.path = temp.getPath();
        a = service.answer(10, 10010, CameraImageSource.DEVICE_ID, false);
        check(a.kind == CameraImageSource.Selection.CANCELLED, "a directory is not an image");
        File text = new File(temp, "notes.jpg");
        Files.write(text.toPath(), "just text, named .jpg".getBytes(StandardCharsets.UTF_8));
        ui.path = text.getPath();
        a = service.answer(10, 10010, CameraImageSource.DEVICE_ID, false);
        check(a.kind == CameraImageSource.Selection.CANCELLED,
                "a file that is not an image is not delivered, whatever its name");
        ui.path = image.getPath();
        ui.refuse = true;
        a = service.answer(10, 10010, CameraImageSource.DEVICE_ID, false);
        check(a.kind == CameraImageSource.Selection.CANCELLED,
                "a selection the document picker would refuse (Android/data...) is not delivered");
        ui.refuse = false;
        check(CameraSourceService.isImage(image), "a JPEG is an image");

        // One question at a time: a second caller while one waits is not asked.
        CameraSourceService.Answer[] second = new CameraSourceService.Answer[1];
        ui.whilePicking = () -> {
            try {
                second[0] = service.answer(11, 10011, CameraImageSource.DEVICE_ID, false);
            } catch (IOException e) {
                throw new AssertionError(e);
            }
        };
        ui.calls.clear();
        a = service.answer(10, 10010, CameraImageSource.DEVICE_ID, false);
        ui.whilePicking = null;
        check(a.kind == CameraImageSource.Selection.IMAGE
                && second[0].kind == CameraImageSource.Selection.CANCELLED
                && ui.calls.equals(List.of("pick:10", "frame:10")),
                "a question while another is open is refused without a dialog");
        a = service.answer(11, 10011, CameraImageSource.DEVICE_ID, false);
        check(a.kind == CameraImageSource.Selection.IMAGE, "the next question is asked once the first is answered");

        callers.attached = false;
        a = service.answer(10, 10010, CameraImageSource.DEVICE_ID, false);
        check(a.kind == CameraImageSource.Selection.CANCELLED,
                "an image chosen after the app's process left is not delivered");
    }

    private static void binderRoundTrip(File temp) throws IOException {
        File image = new File(temp, "photo.png");
        byte[] bytes = "\u0089PNG\r\n\u001a\n and the rest of the file".getBytes(StandardCharsets.ISO_8859_1);
        Files.write(image.toPath(), bytes);
        FakeUi ui = new FakeUi();
        ui.answer = 1;
        ui.path = image.getPath();
        ui.framing = CameraImageSource.Framing.of(0.75f, 0.10f, -0.05f);
        boolean[] enabled = {true};
        CameraSourceService service = new CameraSourceService(new FakeCallers(), ui, () -> enabled[0]);
        ServiceManager.services.put(CameraSourceService.NAME, service);
        try {
            android.os.Binder.callingPid = 10;
            CameraImageSource.Host client = CameraSourceService.CLIENT;
            check(client.enabled(), "the setting reaches the app's process over Binder");
            CameraImageSource.Selection s = client.choose("0", true);
            check(s.kind == CameraImageSource.Selection.IMAGE
                    && java.util.Arrays.equals(bytes, s.image)
                    && Math.abs(s.framing.scale - 0.75f) < 1e-5f
                    && Math.abs(s.framing.offsetX - 0.10f) < 1e-5f
                    && Math.abs(s.framing.offsetY + 0.05f) < 1e-5f,
                    "the selected file's bytes and framing arrive through Binder");
            check(android.os.ParcelFileDescriptor.OPEN.get() == 0,
                    "the descriptor is closed once read");
            ui.answer = 0;
            s = client.choose("0", true);
            check(s.kind == CameraImageSource.Selection.CAMERA, "the camera answer arrives");
            ui.answer = -1;
            s = client.choose("0", true);
            check(s.kind == CameraImageSource.Selection.CANCELLED, "the cancel arrives");
            android.os.Binder.callingPid = 666;
            s = client.choose("0", true);
            check(s.kind == CameraImageSource.Selection.CAMERA,
                    "a refused caller is not asked; its PC camera opens as without the setting");
            s = client.choose(CameraImageSource.DEVICE_ID, false);
            check(s.kind == CameraImageSource.Selection.CANCELLED,
                    "a refused caller gets no selected image");
        } finally {
            ServiceManager.services.remove(CameraSourceService.NAME);
            android.os.Binder.callingPid = 0;
        }
        CameraImageSource.Selection none = CameraSourceService.CLIENT.choose("0", true);
        check(none.kind == CameraImageSource.Selection.CAMERA,
                "without the service a PC camera opens as without the setting");
        none = CameraSourceService.CLIENT.choose(CameraImageSource.DEVICE_ID, false);
        check(none.kind == CameraImageSource.Selection.CANCELLED,
                "without the service there is no selected image");
    }

    /** A scripted server side for CameraImageSource. */
    static final class FakeHost implements CameraImageSource.Host {
        boolean enabled = true;
        CameraImageSource.Selection next;
        int asked;

        @Override public boolean enabled() { return enabled; }

        @Override public CameraImageSource.Selection choose(String cameraId, boolean physical) {
            asked++;
            return next;
        }
    }

    private static void clientChoices() throws Exception {
        FakeHost host = new FakeHost();
        CameraImageSource.Host saved = CameraImageSource.sHost;
        CameraImageSource.sHost = host;
        try {
            List<WindowsCamera.Device> none = List.of();
            check(CameraImageSource.withPicker(none, false).isEmpty(), "setting off: no extra camera");
            List<WindowsCamera.Device> list = CameraImageSource.withPicker(none, true);
            check(list.size() == 1 && CameraImageSource.DEVICE_ID.equals(list.get(0).id)
                    && list.get(0).isRearFacing() && !list.get(0).autofocus,
                    "setting on: a fixed-focus rear camera, last");

            byte[] encoded = {1, 2, 3, 4};
            host.next = new CameraImageSource.Selection(CameraImageSource.Selection.IMAGE, encoded);
            CameraImageSource.Choice choice = new CameraImageSource.Choice();
            WindowsCamera.Capture first = CameraImageSource.open(CameraImageSource.DEVICE_ID, 641, 479, choice);
            check(first.width == 640 && first.height == 478, "an image frame has even dimensions");
            first.close();
            WindowsCamera.Capture picture = CameraImageSource.open(CameraImageSource.DEVICE_ID, 1280, 960, choice);
            check(picture.width == 1280 && host.asked == 1,
                    "a picture or another size keeps the client's image: asked once");
            picture.close();
            int decodes = android.graphics.BitmapFactory.decodes;
            CameraImageSource.open(CameraImageSource.DEVICE_ID, 1280, 960, choice).close();
            check(android.graphics.BitmapFactory.decodes == decodes,
                    "the same size reuses the image's frame instead of decoding it again");
            check(choice.image(), "the client's frames are an image (fixed focus)");
            CameraImageSource.open(CameraImageSource.DEVICE_ID, 640, 480, new CameraImageSource.Choice()).close();
            check(host.asked == 2, "a new client is asked again");

            host.asked = 0;
            host.next = CameraImageSource.Selection.NONE;
            CameraImageSource.Choice cancelled = new CameraImageSource.Choice();
            for (int i = 0; i < 3; i++) {
                int hr = 0;
                try {
                    CameraImageSource.open(CameraImageSource.DEVICE_ID, 640, 480, cancelled);
                } catch (WindowsCamera.OpenException e) {
                    hr = e.hr;
                }
                check(hr == CameraImageSource.ERROR_CANCELLED, "a cancelled client's opens fail");
            }
            check(host.asked == 1, "a cancel is not asked again on every frame or reopen");
            check(!cancelled.image(), "a cancelled client has no image");
            cancelled.resetCancelled();
            host.next = CameraImageSource.Selection.USE_CAMERA;
            check(CameraImageSource.decide("0", cancelled).kind == CameraImageSource.Selection.CAMERA
                    && host.asked == 2, "after a reset (the app starts the preview again) it is asked again");
            cancelled.resetCancelled();
            CameraImageSource.decide("0", cancelled);
            check(host.asked == 2, "a reset forgets only a cancel");

            host.asked = 0;
            host.next = CameraImageSource.Selection.USE_CAMERA;
            CameraImageSource.Selection s = CameraImageSource.decide(CameraImageSource.DEVICE_ID,
                    new CameraImageSource.Choice());
            check(s.kind == CameraImageSource.Selection.CANCELLED,
                    "the selected-image camera has no camera to fall back to");

            host.asked = 0;
            host.enabled = false;
            s = CameraImageSource.decide("0", new CameraImageSource.Choice());
            check(s.kind == CameraImageSource.Selection.CAMERA && host.asked == 0,
                    "setting off: a PC camera is opened without asking");
            host.enabled = true;
            host.next = CameraImageSource.Selection.USE_CAMERA;
            CameraImageSource.Choice camera = new CameraImageSource.Choice();
            CameraImageSource.decide("0", camera);
            s = CameraImageSource.decide("0", camera);
            check(s.kind == CameraImageSource.Selection.CAMERA && host.asked == 1,
                    "the camera answer is kept for the client");
        } finally {
            CameraImageSource.sHost = saved;
        }
    }

    private static void frames() throws Exception {
        byte[] nv12 = CameraImageSource.toNv12(new byte[] {1, 2, 3}, 64, 48);
        check(nv12.length == 64 * 48 * 3 / 2, "NV12 is width*height*3/2 bytes");
        boolean any = false;
        for (byte b : nv12) any |= b != 0;
        check(any, "the frame carries the image");
        android.media.ExifInterface.orientation = android.media.ExifInterface.ORIENTATION_ROTATE_90;
        int transformed = android.graphics.Bitmap.transformed;
        CameraImageSource.toNv12(new byte[] {1, 2, 3}, 64, 48);
        check(android.graphics.Bitmap.transformed == transformed + 1,
                "an EXIF orientation is applied before scaling, as Windows shows the photo");
        android.media.ExifInterface.orientation = android.media.ExifInterface.ORIENTATION_NORMAL;
        CameraImageSource.toNv12(new byte[] {1, 2, 3}, 64, 48);
        check(android.graphics.Bitmap.transformed == transformed + 1, "an upright image is not transformed");

        // Framing zoom-out and pan: 320x240 into 64x48 (base scale 0.2) with 0.5x zoom and +0.25x, -0.25y pan.
        CameraImageSource.Framing zoomedOut = CameraImageSource.Framing.of(0.5f, 0.25f, -0.25f);
        CameraImageSource.toNv12(new byte[] {1, 2, 3}, 64, 48, zoomedOut);
        android.graphics.RectF dst = android.graphics.Canvas.lastDst;
        check(dst != null
                && Math.abs((dst.right - dst.left) - 32f) < 1e-3f
                && Math.abs((dst.bottom - dst.top) - 24f) < 1e-3f
                && Math.abs(dst.left - 32f) < 1e-3f
                && Math.abs(dst.top - 0f) < 1e-3f,
                "framing scale and pan position the image on black margins");
        CameraImageSource.Framing clamped = CameraImageSource.Framing.of(Float.NaN, 5f, -5f);
        check(clamped.scale == 1.0f && clamped.offsetX == 1.0f && clamped.offsetY == -1.0f,
                "invalid or out-of-range framing values are clamped");
        int[] quad = {0xFFFF0000, 0xFF0000FF, 0xFF0000FF, 0xFFFF0000};
        byte[] q = CameraImageSource.argbToNv12(quad, 2, 2);
        byte[] red = CameraImageSource.argbToNv12(new int[] {0xFFFF0000, 0xFFFF0000, 0xFFFF0000, 0xFFFF0000}, 2, 2);
        check((q[4] & 0xFF) != (red[4] & 0xFF), "chroma averages its 2x2 block");
        int[] white = new int[4];
        java.util.Arrays.fill(white, 0xFFFFFFFF);
        byte[] w = CameraImageSource.argbToNv12(white, 2, 2);
        check((w[0] & 0xFF) == 235 && (w[4] & 0xFF) == 128 && (w[5] & 0xFF) == 128,
                "white is Y 235, U and V 128 (BT.601 limited range)");
        boolean rejected = false;
        try {
            CameraImageSource.toNv12(new byte[] {1}, 63, 48);
        } catch (IllegalArgumentException expected) {
            rejected = true;
        }
        check(rejected, "odd NV12 dimensions are refused");
        boolean undecodable = false;
        try {
            CameraImageSource.toNv12(new byte[0], 64, 48);
        } catch (IOException expected) {
            undecodable = true;
        }
        check(undecodable, "an image Android cannot decode is an error");

        WindowsCamera.Capture capture = WindowsCamera.Capture.ofFrame(nv12, 64, 48);
        byte[] frame = new byte[nv12.length];
        check(capture.read(frame) == 0 && java.util.Arrays.equals(nv12, frame),
                "each read is the image's frame");
        check(capture.focus(WindowsCamera.Capture.FOCUS_GET, null) == 0x80004001,
                "an image has no focus control (E_NOTIMPL)");
        capture.close();
        check(capture.read(frame) == 0x80004004, "a closed image capture reads E_ABORT");
        boolean mismatch = false;
        try {
            WindowsCamera.Capture.ofFrame(new byte[10], 64, 48);
        } catch (IllegalArgumentException expected) {
            mismatch = true;
        }
        check(mismatch, "a frame shorter than its size is refused");
    }
}
