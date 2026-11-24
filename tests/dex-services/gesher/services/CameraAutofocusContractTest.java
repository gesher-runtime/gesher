/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.pm.FeatureInfo;
import android.content.pm.PackageManager;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraMetadata;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.impl.CameraMetadataNative;
import android.util.ArrayMap;
import android.util.Size;
import com.gesher.server.CameraAutoFocus;
import com.gesher.server.PackageManagerService;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Autofocus contracts: which devices report autofocus (the natives' device lines), the
 * FEATURE_CAMERA_AUTOFOCUS rule (the rear camera's real autofocus only), the Camera2
 * characteristics and default requests of focusing and fixed-focus cameras, the legacy
 * focus modes, and CameraAutoFocus' state machine against Android's CONTROL_AF_STATE
 * transition table. The lens here is an isolated fixture (scripted positions and picture
 * sharpness): no Windows camera and no driver focus is exercised.
 */
public final class CameraAutofocusContractTest {
    private static int checks;

    private static void check(boolean value, String message) {
        checks++;
        if (!value) throw new AssertionError(message);
    }

    private static Method method(Class<?> type, String name, Class<?>... parameters)
            throws Exception {
        Method m = type.getDeclaredMethod(name, parameters);
        m.setAccessible(true);
        return m;
    }

    private static Object device(String id, boolean rear, boolean autofocus) throws Exception {
        Class<?> deviceClass = Class.forName("com.gesher.server.WindowsCamera$Device");
        Constructor<?> constructor = deviceClass.getDeclaredConstructor(String.class,
                String.class, List.class, boolean.class, boolean.class);
        constructor.setAccessible(true);
        return constructor.newInstance(id, "fixture " + id, List.of(new Size(640, 480)), rear,
                autofocus);
    }

    private static boolean bool(Object device, String name) throws Exception {
        Field f = device.getClass().getDeclaredField(name);
        f.setAccessible(true);
        return f.getBoolean(device);
    }

    private static boolean rear(Object device) throws Exception {
        return (Boolean) device.getClass().getMethod("isRearFacing").invoke(device);
    }

    // --- the natives' device lines -------------------------------------------------------

    private static void deviceLines() throws Exception {
        Class<?> windowsCamera = Class.forName("com.gesher.server.WindowsCamera");
        Method resolve = method(windowsCamera, "resolveDevices", String[].class, String.class,
                boolean.class, boolean.class);
        Method key = method(windowsCamera, "selectionKey", String.class);
        String rearId = "\\\\?\\usb#vid_fixture&pid_af";
        String t = "\t";
        String[] lines = {
                rearId + t + "AF camera" + t + "1280x720,640x480" + t + "af",
                "front-fixed" + t + "Fixed camera" + t + "640x480" + t,
                "front-old-natives" + t + "Old line" + t + "640x480",
                "gesher-test" + t + "Gesher Test Camera" + t + "640x480,320x240" + t + "af",
        };
        @SuppressWarnings("unchecked")
        List<Object> devices = (List<Object>) resolve.invoke(null, lines,
                key.invoke(null, rearId), true, true);
        check(devices.size() == 4, "all four device lines parse: " + devices.size());
        check(bool(devices.get(0), "autofocus") && rear(devices.get(0)),
                "the af flag marks real autofocus on the selected rear camera");
        check(!bool(devices.get(1), "autofocus"), "an empty flag list is fixed focus");
        check(!bool(devices.get(2), "autofocus"), "a line without flags is fixed focus");
        check(!bool(devices.get(3), "autofocus") && rear(devices.get(3)),
                "the test camera keeps fixed focus whatever its line says");
    }

    // --- FEATURE_CAMERA_AUTOFOCUS ---------------------------------------------------------

    @SuppressWarnings("unchecked")
    private static ArrayMap<String, FeatureInfo> features(Object... devices) {
        ArrayMap<String, FeatureInfo> features = new ArrayMap<>();
        List<Object> list = new ArrayList<>(Arrays.asList(devices));
        PackageManagerService.addCameraFeatures(features,
                (List<com.gesher.server.WindowsCamera.Device>) (List<?>) list);
        return features;
    }

    private static void featureRule() throws Exception {
        String af = PackageManager.FEATURE_CAMERA_AUTOFOCUS;
        ArrayMap<String, FeatureInfo> f = features(device("rear-af", true, true));
        check(f.containsKey(PackageManager.FEATURE_CAMERA) && f.containsKey(af),
                "a rear camera with real autofocus reports camera.autofocus");
        f = features(device("rear-fixed", true, false));
        check(f.containsKey(PackageManager.FEATURE_CAMERA) && !f.containsKey(af),
                "a fixed-focus rear camera does not report camera.autofocus");
        f = features(device("front-af", false, true));
        check(f.containsKey(PackageManager.FEATURE_CAMERA_FRONT) && !f.containsKey(af)
                        && !f.containsKey(PackageManager.FEATURE_CAMERA),
                "a front camera's autofocus does not report the rear camera's feature");
        f = features(device("front-af", false, true), device("rear-fixed", true, false));
        check(!f.containsKey(af), "autofocus on another camera than the rear one is not claimed");
        f = features(device("gesher-test", true, true));
        check(!f.containsKey(af), "the test camera never reports autofocus");
        f = features();
        check(f.isEmpty(), "no cameras, no camera features");
    }

    // --- Camera2 characteristics and requests ---------------------------------------------

    private static Set<Integer> set(int[] values) {
        Set<Integer> s = new HashSet<>();
        for (int v : values) s.add(v);
        return s;
    }

    private static void characteristics() throws Exception {
        Class<?> service = Class.forName("com.gesher.server.CameraService");
        Class<?> deviceClass = Class.forName("com.gesher.server.WindowsCamera$Device");
        Method characteristics = method(service, "characteristics", deviceClass);
        Method defaultRequest = method(service, "defaultRequest", deviceClass, int.class);

        Object focusing = device("rear-af", true, true);
        CameraMetadataNative c = (CameraMetadataNative) characteristics.invoke(null, focusing);
        Set<Integer> modes = set(c.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES));
        check(modes.equals(Set.of(CameraMetadata.CONTROL_AF_MODE_AUTO,
                        CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO,
                        CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_PICTURE)),
                "a focusing camera offers exactly the modes run: " + modes);
        check(c.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) > 0f,
                "a focusing lens is not reported as fixed focus");
        check(c.get(CameraCharacteristics.LENS_INFO_FOCUS_DISTANCE_CALIBRATION)
                        == CameraMetadata.LENS_INFO_FOCUS_DISTANCE_CALIBRATION_UNCALIBRATED,
                "focus distances stay uncalibrated");
        check(Arrays.equals(c.get(CameraCharacteristics.CONTROL_MAX_REGIONS), new int[3]),
                "no AF regions are claimed");
        int[][] templates = {
                {1 /* PREVIEW */, CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_PICTURE},
                {2 /* STILL_CAPTURE */, CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_PICTURE},
                {3 /* RECORD */, CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO},
                {4 /* VIDEO_SNAPSHOT */, CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO},
        };
        for (int[] t : templates) {
            CameraMetadataNative r = (CameraMetadataNative) defaultRequest.invoke(null, focusing,
                    t[0]);
            int mode = r.get(CaptureRequest.CONTROL_AF_MODE);
            check(mode == t[1] && modes.contains(mode),
                    "template " + t[0] + " defaults to an offered continuous mode: " + mode);
        }

        Object fixed = device("rear-fixed", true, false);
        c = (CameraMetadataNative) characteristics.invoke(null, fixed);
        check(Arrays.equals(c.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES),
                        new int[] {CameraMetadata.CONTROL_AF_MODE_OFF}),
                "a fixed-focus camera keeps AF_MODE_OFF alone");
        check(c.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) == 0f,
                "a fixed-focus lens reports 0");
        for (int[] t : templates) {
            CameraMetadataNative r = (CameraMetadataNative) defaultRequest.invoke(null, fixed,
                    t[0]);
            check(r.get(CaptureRequest.CONTROL_AF_MODE) == CameraMetadata.CONTROL_AF_MODE_OFF,
                    "a fixed-focus template keeps AF off");
        }
    }

    // --- the legacy API --------------------------------------------------------------------

    @SuppressWarnings("unchecked")
    private static Map<String, String> legacyParameters(Object device) throws Exception {
        Class<?> legacy = Class.forName("com.gesher.server.LegacyCamera");
        Class<?> sessionClass = Class.forName("com.gesher.server.LegacyCamera$Session");
        Constructor<?> constructor = sessionClass.getDeclaredConstructor(Object.class,
                device.getClass());
        constructor.setAccessible(true);
        Object session = constructor.newInstance(new Object(), device);
        method(legacy, "defaults", sessionClass).invoke(null, session);
        Field params = sessionClass.getDeclaredField("params");
        params.setAccessible(true);
        return (Map<String, String>) params.get(session);
    }

    private static void legacy() throws Exception {
        Map<String, String> p = legacyParameters(device("rear-af", true, true));
        check("auto".equals(p.get("focus-mode")), "a focusing camera starts in focus mode auto");
        check(Set.of(p.get("focus-mode-values").split(",")).equals(
                        Set.of("auto", "continuous-video", "continuous-picture")),
                "legacy focus modes match Camera2's: " + p.get("focus-mode-values"));
        p = legacyParameters(device("rear-fixed", true, false));
        check("fixed".equals(p.get("focus-mode")) && "fixed".equals(p.get("focus-mode-values")),
                "a fixed-focus camera keeps focus mode fixed");
        Method afMode = method(Class.forName("com.gesher.server.LegacyCamera"), "afMode",
                String.class);
        check((int) afMode.invoke(null, "auto") == CameraMetadata.CONTROL_AF_MODE_AUTO
                        && (int) afMode.invoke(null, "continuous-picture")
                                == CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_PICTURE
                        && (int) afMode.invoke(null, "continuous-video")
                                == CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO
                        && (int) afMode.invoke(null, "fixed") == CameraMetadata.CONTROL_AF_MODE_OFF,
                "legacy focus modes map to the Camera2 modes run");
    }

    // --- the state machine ---------------------------------------------------------------

    /** The driver's focus, scripted: what the lens reports and what Gesher asked of it. */
    private static final class Lens implements CameraAutoFocus.Lens {
        boolean autoOn;
        boolean failAuto;
        int autoCalls, lockCalls, positionCalls;
        Integer position = 100;

        @Override
        public boolean auto() {
            autoCalls++;
            if (failAuto) return false;
            autoOn = true;
            return true;
        }

        @Override
        public boolean lock() {
            lockCalls++;
            autoOn = false;
            return true;
        }

        @Override
        public Integer position() {
            positionCalls++;
            return position;
        }
    }

    private static final int IDLE = CameraMetadata.CONTROL_AF_TRIGGER_IDLE;
    private static final int START = CameraMetadata.CONTROL_AF_TRIGGER_START;
    private static final int CANCEL = CameraMetadata.CONTROL_AF_TRIGGER_CANCEL;
    private static final int INACTIVE = CameraMetadata.CONTROL_AF_STATE_INACTIVE;
    private static final int PASSIVE_SCAN = CameraMetadata.CONTROL_AF_STATE_PASSIVE_SCAN;
    private static final int PASSIVE_FOCUSED = CameraMetadata.CONTROL_AF_STATE_PASSIVE_FOCUSED;
    private static final int ACTIVE_SCAN = CameraMetadata.CONTROL_AF_STATE_ACTIVE_SCAN;
    private static final int FOCUSED_LOCKED = CameraMetadata.CONTROL_AF_STATE_FOCUSED_LOCKED;
    private static final int NOT_FOCUSED_LOCKED =
            CameraMetadata.CONTROL_AF_STATE_NOT_FOCUSED_LOCKED;
    private static final int PASSIVE_UNFOCUSED =
            CameraMetadata.CONTROL_AF_STATE_PASSIVE_UNFOCUSED;

    /**
     * CaptureResult.CONTROL_AF_STATE's transition tables: (mode, from, to) that Android
     * allows within one mode (staying in a state is always allowed).
     */
    private static boolean allowed(int mode, int from, int to) {
        if (from == to) return true;
        if (mode == CameraMetadata.CONTROL_AF_MODE_AUTO) {
            switch (from) {
                case INACTIVE: return to == ACTIVE_SCAN || to == NOT_FOCUSED_LOCKED;
                case ACTIVE_SCAN:
                    return to == FOCUSED_LOCKED || to == NOT_FOCUSED_LOCKED || to == INACTIVE;
                case FOCUSED_LOCKED:
                case NOT_FOCUSED_LOCKED:
                    return to == ACTIVE_SCAN || to == INACTIVE;
                default: return false;
            }
        }
        switch (from) {
            case INACTIVE:
                return to == PASSIVE_SCAN || to == FOCUSED_LOCKED || to == NOT_FOCUSED_LOCKED;
            case PASSIVE_SCAN:
                return to == PASSIVE_FOCUSED || to == PASSIVE_UNFOCUSED || to == INACTIVE
                        || to == FOCUSED_LOCKED || to == NOT_FOCUSED_LOCKED;
            case PASSIVE_FOCUSED:
            case PASSIVE_UNFOCUSED:
                return to == PASSIVE_SCAN || to == INACTIVE || to == FOCUSED_LOCKED
                        || to == NOT_FOCUSED_LOCKED;
            case FOCUSED_LOCKED:
            case NOT_FOCUSED_LOCKED:
                return to == INACTIVE;
            default: return false;
        }
    }

    /** Drives a machine frame by frame (33 ms apart), checking every transition. */
    private static final class Run {
        final Lens lens = new Lens();
        final CameraAutoFocus af = new CameraAutoFocus(lens);
        long now = 1000;
        int mode = -1;
        int state = INACTIVE;
        final List<Integer> states = new ArrayList<>();

        int frame(int afMode, int trigger, double sharpness) {
            now += 33;
            int next = af.frame(afMode, trigger, now, sharpness);
            if (afMode == mode) {
                check(allowed(afMode, state, next), "mode " + afMode + ": transition " + state
                        + " -> " + next + " is not Android's");
            } else {
                check(next == INACTIVE || next == NOT_FOCUSED_LOCKED || next == ACTIVE_SCAN
                                || next == FOCUSED_LOCKED,
                        "a mode change starts from INACTIVE: " + next);
            }
            mode = afMode;
            state = next;
            states.add(next);
            return next;
        }

        /** Frames until {@code until} or {@code limit} frames. */
        int frames(int afMode, int count, double sharpness, Integer position) {
            int s = state;
            for (int i = 0; i < count; i++) {
                if (position != null) lens.position = position;
                s = frame(afMode, IDLE, sharpness);
            }
            return s;
        }
    }

    private static void autoMode() {
        int auto = CameraMetadata.CONTROL_AF_MODE_AUTO;
        Run r = new Run();
        check(r.frame(auto, IDLE, 0.5) == INACTIVE, "AUTO starts INACTIVE");
        check(!r.lens.autoOn && r.lens.lockCalls == 1, "AUTO keeps the lens still until a trigger");
        r.frames(auto, 30, 0.5, null);
        check(r.state == INACTIVE && r.lens.positionCalls == 0,
                "no scan and no driver queries without a trigger");
        check(r.frame(auto, START, 0.5) == ACTIVE_SCAN && r.lens.autoOn && r.af.lensMoving(),
                "START scans with the driver's autofocus on");
        long started = r.now;
        // The lens travels for 10 frames, the picture sharpening, then rests.
        for (int i = 0; i < 10; i++) {
            r.lens.position = 100 + i * 5;
            check(r.frame(auto, IDLE, 0.1 + i * 0.04) == ACTIVE_SCAN, "moving lens: scanning");
        }
        int s = r.state;
        long locked = 0;
        for (int i = 0; i < 60 && s == ACTIVE_SCAN; i++) {
            s = r.frame(auto, IDLE, i % 2 == 0 ? 0.50 : 0.49);
            if (!r.lens.autoOn && locked == 0) locked = r.now;
        }
        check(s == FOCUSED_LOCKED, "a converged scan ends FOCUSED_LOCKED: " + s);
        check(!r.lens.autoOn && !r.af.lensMoving(), "the lens is locked after the scan");
        check(locked - started >= CameraAutoFocus.MIN_SCAN_MS,
                "convergence is observed, not assumed: locked after " + (locked - started) + " ms");
        r.frames(auto, 10, 0.5, null);
        check(r.state == FOCUSED_LOCKED, "a locked state stays without triggers");
        check(r.frame(auto, CANCEL, 0.5) == INACTIVE, "CANCEL returns to INACTIVE");
        check(r.frame(auto, START, 0.5) == ACTIVE_SCAN, "a new START scans again");
        check(r.frame(auto, CANCEL, 0.5) == INACTIVE && !r.lens.autoOn,
                "CANCEL during a scan stops it with the lens locked");

        // A hunting lens never converges: NOT_FOCUSED_LOCKED after MAX_SCAN_MS, never FOCUSED.
        r = new Run();
        r.frame(auto, IDLE, 0.5);
        r.frame(auto, START, 0.5);
        started = r.now;
        s = ACTIVE_SCAN;
        for (int i = 0; s == ACTIVE_SCAN && i < 400; i++) {
            r.lens.position = 100 + (i % 7) * 10;
            s = r.frame(auto, IDLE, 0.2 + (i % 5) * 0.05);
        }
        check(s == NOT_FOCUSED_LOCKED && r.now - started >= CameraAutoFocus.MAX_SCAN_MS
                        && !r.lens.autoOn,
                "a scan that never converges fails, locked, after " + (r.now - started) + " ms");

        // A featureless (black) picture cannot be judged in focus.
        r = new Run();
        r.frame(auto, IDLE, 0);
        r.frame(auto, START, 0);
        s = r.frames(auto, 200, 0, null);
        check(s == NOT_FOCUSED_LOCKED, "a black picture never reads as focused: " + s);

        // The lens settles, but locking it loses the sharpness: not focused.
        r = new Run();
        r.frame(auto, IDLE, 0.5);
        r.frame(auto, START, 0.5);
        s = ACTIVE_SCAN;
        for (int i = 0; s == ACTIVE_SCAN && i < 200; i++) {
            s = r.frame(auto, IDLE, r.lens.autoOn ? 0.5 : 0.2);
        }
        check(s == NOT_FOCUSED_LOCKED, "a lock that blurs the picture is reported: " + s);

        // The driver refuses automatic focus: the trigger fails at once, honestly.
        r = new Run();
        r.lens.failAuto = true;
        r.frame(auto, IDLE, 0.5);
        check(r.frame(auto, START, 0.5) == NOT_FOCUSED_LOCKED, "no driver autofocus, no scan");
    }

    private static int settle(Run r, int mode, int until) {
        int s = r.state;
        for (int i = 0; i < 100 && s != until; i++) s = r.frame(mode, IDLE, 0.5);
        return s;
    }

    private static void continuousModes() {
        int picture = CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_PICTURE;
        int video = CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO;
        Run r = new Run();
        check(r.frame(picture, IDLE, 0.5) == INACTIVE && r.lens.autoOn,
                "a continuous mode turns the driver's autofocus on, INACTIVE at first");
        check(r.frame(picture, IDLE, 0.5) == PASSIVE_SCAN && r.af.lensMoving(),
                "then the driver's scan is PASSIVE_SCAN");
        check(settle(r, picture, PASSIVE_FOCUSED) == PASSIVE_FOCUSED && r.lens.autoOn,
                "a still lens and picture: PASSIVE_FOCUSED, autofocus still on");
        r.lens.position = 140;
        int moved = PASSIVE_FOCUSED;
        // The position is read at most every POSITION_INTERVAL_MS.
        for (int i = 0; i < 5 && moved == PASSIVE_FOCUSED; i++) moved = r.frame(picture, IDLE, 0.5);
        check(moved == PASSIVE_SCAN, "the lens moving again is a new scan");
        check(settle(r, picture, PASSIVE_FOCUSED) == PASSIVE_FOCUSED, "and it converges again");
        check(r.frame(picture, IDLE, 0.2) == PASSIVE_SCAN, "a scene change is a new scan");
        check(settle(r, picture, PASSIVE_FOCUSED) == PASSIVE_FOCUSED, "converged");
        int locks = r.lens.lockCalls;
        check(r.frame(picture, START, 0.5) == FOCUSED_LOCKED && r.lens.lockCalls == locks + 1
                        && !r.lens.autoOn,
                "START when focused locks at once: FOCUSED_LOCKED");
        r.frames(picture, 20, 0.3, 200);
        check(r.state == FOCUSED_LOCKED, "a lock holds whatever the scene does");
        check(r.frame(picture, CANCEL, 0.5) == INACTIVE && r.lens.autoOn,
                "CANCEL resumes the driver's continuous autofocus");

        // START during a CONTINUOUS_PICTURE scan waits for it to end.
        r = new Run();
        r.frame(picture, IDLE, 0.5);
        r.frame(picture, IDLE, 0.5);
        check(r.frame(picture, START, 0.5) == PASSIVE_SCAN && r.lens.autoOn,
                "CONTINUOUS_PICTURE: START during a scan lets it finish");
        check(settle(r, picture, FOCUSED_LOCKED) == FOCUSED_LOCKED && !r.lens.autoOn,
                "then locks: FOCUSED_LOCKED");

        // CONTINUOUS_VIDEO locks at once, even mid-scan: not focused.
        r = new Run();
        r.frame(video, IDLE, 0.5);
        r.frame(video, IDLE, 0.5);
        check(r.frame(video, START, 0.5) == NOT_FOCUSED_LOCKED && !r.lens.autoOn,
                "CONTINUOUS_VIDEO: START mid-scan locks at once, NOT_FOCUSED_LOCKED");

        // A trigger with the mode change (CameraX's focus request): INACTIVE -> locked.
        r = new Run();
        check(r.frame(picture, START, 0.5) == NOT_FOCUSED_LOCKED,
                "START before any scan: NOT_FOCUSED_LOCKED");

        // A scan that never converges: PASSIVE_UNFOCUSED, and START locks not focused.
        r = new Run();
        r.frame(picture, IDLE, 0.5);
        int s = INACTIVE;
        for (int i = 0; s != PASSIVE_UNFOCUSED && i < 400; i++) {
            r.lens.position = 100 + (i % 3);
            s = r.frame(picture, IDLE, 0.3);
        }
        check(s == PASSIVE_UNFOCUSED, "a continuous scan that never converges is UNFOCUSED");
        check(r.frame(picture, START, 0.3) == NOT_FOCUSED_LOCKED,
                "START when unfocused: NOT_FOCUSED_LOCKED");

        // Leaving autofocus modes: the lens stays still, INACTIVE.
        r = new Run();
        r.frame(picture, IDLE, 0.5);
        int queries = r.lens.positionCalls;
        check(r.frame(CameraMetadata.CONTROL_AF_MODE_OFF, IDLE, 0.5) == INACTIVE
                        && !r.lens.autoOn,
                "AF_MODE_OFF stops the driver's autofocus");
        r.frames(CameraMetadata.CONTROL_AF_MODE_OFF, 10, 0.5, null);
        check(r.lens.positionCalls == queries, "AF off asks the driver nothing per frame");
    }

    private static void sharpness() {
        int w = 64, h = 48;
        byte[] sharp = new byte[w * h * 3 / 2];
        byte[] soft = new byte[w * h * 3 / 2];
        byte[] flat = new byte[w * h * 3 / 2];
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                sharp[y * w + x] = (byte) (((x / 2 + y / 2) % 2 == 0) ? 200 : 40);
                soft[y * w + x] = (byte) (120 + 10 * Math.sin(x / 6.0));
                flat[y * w + x] = (byte) 120;
            }
        }
        double s = CameraAutoFocus.sharpness(sharp, w, h);
        double b = CameraAutoFocus.sharpness(soft, w, h);
        check(s > b && b > 0, "fine detail reads sharper than a blurred picture: " + s + " " + b);
        check(CameraAutoFocus.sharpness(flat, w, h) == 0, "a featureless picture has no sharpness");
        check(CameraAutoFocus.sharpness(new byte[w * h * 3 / 2], w, h) == 0,
                "a black picture has no sharpness");
    }

    public static void main(String[] args) throws Exception {
        deviceLines();
        featureRule();
        characteristics();
        legacy();
        autoMode();
        continuousModes();
        sharpness();
        System.out.println("CameraAutofocusContractTest: device flags, rear-only autofocus"
                + " feature, AF modes, legacy focus modes and Android AF state transitions"
                + " passed (" + checks + " checks)");
    }
}
