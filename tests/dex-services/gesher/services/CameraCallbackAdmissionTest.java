/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.graphics.ImageFormat;
import android.graphics.SurfaceTexture;
import android.hardware.ICameraService;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.ICameraDeviceCallbacks;
import android.hardware.camera2.ICameraDeviceUser;
import android.hardware.camera2.impl.CameraMetadataNative;
import android.hardware.camera2.impl.CaptureResultExtras;
import android.hardware.camera2.params.DynamicRangeProfiles;
import android.hardware.camera2.params.InputConfiguration;
import android.hardware.camera2.params.OutputConfiguration;
import android.hardware.camera2.params.SessionConfiguration;
import android.media.ImageReader;
import android.os.ServiceSpecificException;
import android.util.Size;
import java.lang.reflect.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;

/** Real ART/native metadata and actual owners; capture scheduling is explicitly held. */
public final class CameraCallbackAdmissionTest {
    private static int checks;
    private static Class<?> cameraType, queueType;
    private static Constructor<?> constructor;
    private static Object service;

    private static synchronized void require(boolean good, String message) {
        ++checks;
        if (!good) throw new AssertionError(message);
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static Object get(Object instance, String name) throws Exception {
        Object lock;
        try { lock = field(instance.getClass(), "mLock").get(instance); }
        catch (NoSuchFieldException missing) { return field(instance.getClass(), name).get(instance); }
        synchronized (lock) { return field(instance.getClass(), name).get(instance); }
    }
    private static long number(Object instance, String name) throws Exception {
        return ((Number) get(instance, name)).longValue();
    }
    private static Object call(Object instance, String name, Class<?>[] types, Object... args)
            throws Exception {
        Method method = instance.getClass().getMethod(name, types);
        try { return method.invoke(instance, args); }
        catch (InvocationTargetException e) {
            Throwable cause = e.getCause();
            if (cause instanceof Exception) throw (Exception) cause;
            throw (Error) cause;
        }
    }
    private static Object camera(ICameraDeviceCallbacks callbacks) throws Exception {
        Object c = constructor.newInstance(service, "admission-fixture", null, callbacks);
        // No physical source is substituted. This test owns a deliberately paused
        // producer and exercises the production submit/flush/disconnect paths.
        field(cameraType, "mThread").set(c, new Thread());
        return c;
    }
    private static int queues() throws Exception {
        synchronized (queueType) { return field(queueType, "sQueues").getInt(null); }
    }
    private static void retired() throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(15);
        while (queues() != 0 && System.nanoTime() < deadline) Thread.sleep(1);
        require(queues() == 0, "all callback workers and native metadata reservations retire");
    }
    private static CaptureRequest request(int bytes) {
        CameraMetadataNative metadata = new CameraMetadataNative();
        if (bytes > 0) {
            int tag = CameraMetadataNative.getTag("android.jpeg.gpsProcessingMethod");
            metadata.writeValues(tag, new byte[bytes]);
        }
        return new CaptureRequest.Builder(metadata, false, -1, "0", null).build();
    }
    private static void submit(Object camera, CaptureRequest request) throws Exception {
        call(camera, "submitRequest", new Class<?>[] {CaptureRequest.class, boolean.class},
                request, false);
    }
    private static SessionConfiguration session(int mode, OutputConfiguration... outputs) {
        return new SessionConfiguration(mode, List.of(outputs), Runnable::run,
                new CameraCaptureSession.StateCallback() {
                    @Override public void onConfigured(CameraCaptureSession session) {}
                    @Override public void onConfigureFailed(CameraCaptureSession session) {}
                });
    }
    private interface CameraOperation {
        void run() throws Exception;
    }
    private static void rejects(int errorCode, CameraOperation operation, String message)
            throws Exception {
        try {
            operation.run();
        } catch (ServiceSpecificException e) {
            require(e.errorCode == errorCode, message + " uses the camera service error");
            return;
        }
        throw new AssertionError(message);
    }
    private static void checkSessionAdmission() throws Exception {
        ICameraDeviceUser camera = (ICameraDeviceUser) camera(new ICameraDeviceCallbacks.Default());
        try (ImageReader preview = ImageReader.newInstance(640, 480, ImageFormat.YUV_420_888, 2);
                ImageReader still = ImageReader.newInstance(320, 240, ImageFormat.JPEG, 2)) {
            int regular = SessionConfiguration.SESSION_REGULAR;
            int highSpeed = SessionConfiguration.SESSION_HIGH_SPEED;
            int invalid = ICameraService.ERROR_ILLEGAL_ARGUMENT;
            OutputConfiguration yuv = new OutputConfiguration(preview.getSurface());
            OutputConfiguration jpeg = new OutputConfiguration(still.getSurface());
            require(camera.isSessionConfigurationSupported(session(regular, yuv, jpeg)),
                    "the supported YUV plus JPEG combination passes the query");
            int stream = camera.createStream(yuv);
            require(stream >= 0 && camera.createStream(jpeg) >= 0,
                    "the same two outputs pass actual stream admission");
            require(!camera.isSessionConfigurationSupported(session(regular)),
                    "no outputs cannot form a capture session");

            SessionConfiguration input = session(regular, yuv);
            input.setInputConfiguration(new InputConfiguration(640, 480, ImageFormat.YUV_420_888));
            require(!camera.isSessionConfigurationSupported(input), "reprocessing is unavailable");
            rejects(invalid, () -> camera.createInputStream(640, 480, ImageFormat.YUV_420_888, false),
                    "the session cannot admit an input stream");
            rejects(invalid, () -> camera.isSessionConfigurationSupported(session(highSpeed, yuv)),
                    "high speed query on this device is invalid");
            rejects(invalid, () -> camera.endConfigure(highSpeed, null, 0),
                    "high speed configuration is invalid");

            OutputConfiguration deferred = new OutputConfiguration(new Size(640, 480),
                    SurfaceTexture.class);
            require(!camera.isSessionConfigurationSupported(session(regular, deferred)),
                    "deferred output is unavailable");
            rejects(invalid, () -> camera.createStream(deferred),
                    "an output without a Surface cannot be admitted");

            OutputConfiguration physical = new OutputConfiguration(preview.getSurface());
            physical.setPhysicalCameraId("unavailable-physical-camera");
            require(!camera.isSessionConfigurationSupported(session(regular, physical)),
                    "physical-camera routing is unavailable");
            rejects(invalid, () -> camera.createStream(physical),
                    "the stream cannot silently use another camera");

            OutputConfiguration hdr = new OutputConfiguration(preview.getSurface());
            hdr.setDynamicRangeProfile(DynamicRangeProfiles.HLG10);
            require(!camera.isSessionConfigurationSupported(session(regular, hdr)),
                    "HDR output is unavailable");
            rejects(invalid, () -> camera.updateOutputConfiguration(stream, hdr),
                    "the update cannot silently substitute SDR");
            camera.updateOutputConfiguration(stream, yuv);
        } finally {
            camera.disconnect();
        }
        rejects(ICameraService.ERROR_DISCONNECTED,
                () -> camera.isSessionConfigurationSupported(session(SessionConfiguration.SESSION_REGULAR)),
                "closed devices cannot answer a session query");
        retired();
    }
    private static final class Blocked extends ICameraDeviceCallbacks.Default {
        final CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        final AtomicInteger terminals = new AtomicInteger();
        final List<Long> frames = Collections.synchronizedList(new ArrayList<>());
        @Override public void onDeviceError(int error, CaptureResultExtras extras) {
            entered.countDown();
            try {
                require(release.await(15, TimeUnit.SECONDS), "blocked client released by fixture");
            } catch (InterruptedException e) { throw new AssertionError(e); }
            if (extras.getFrameNumber() >= 0) {
                require(error == ERROR_CAMERA_REQUEST, "flush returns capture failure, not fabricated success");
                frames.add(extras.getFrameNumber());
                terminals.incrementAndGet();
            }
        }
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        cameraType = Class.forName("com.gesher.server.CameraDeviceUser");
        queueType = Class.forName("com.gesher.server.CameraCallbacks");
        constructor = cameraType.getDeclaredConstructors()[0]; constructor.setAccessible(true);
        Class<?> serviceType = Class.forName("com.gesher.server.CameraService");
        service = serviceType.getMethod("get").invoke(null);
        checkSessionAdmission();
        int maximum = field(queueType, "MAX_FRAMES").getInt(null);
        long budget = field(queueType, "MAX_BYTES").getLong(null);
        Blocked target = new Blocked(); Object camera = camera(target);
        CaptureRequest ordinary = request(0);
        for (int i = 0; i < maximum; ++i) submit(camera, ordinary);
        long next = number(camera, "mNextFrameNumber"), id = number(camera, "mNextRequestId");
        for (int i = 0; i < 256; ++i) {
            try { submit(camera, ordinary); throw new AssertionError("unbounded admission"); }
            catch (ServiceSpecificException e) {
                require(e.errorCode == ICameraService.ERROR_INVALID_OPERATION, "Android rejection status");
            }
        }
        require(number(camera, "mNextFrameNumber") == next
                && number(camera, "mNextRequestId") == id, "rejected work consumes no IDs");
        Object queue = get(camera, "mCallbackQueue");
        require(number(queue, "mBytes") <= budget, "native-metadata reservation stays within budget");
        call(camera, "flush", new Class<?>[0]);
        require(target.entered.await(5, TimeUnit.SECONDS), "actual callback worker is blocked");
        require(number(queue, "mFrames") == maximum, "executing callback remains charged");
        call(camera, "disconnect", new Class<?>[0]);
        require(queues() == 1, "closed blocked callback remains charged");
        target.release.countDown(); retired();
        require(target.terminals.get() == maximum, "every admitted request completed exactly once");
        for (int i = 0; i < maximum; ++i) require(target.frames.get(i) == i, "flush callback order");

        Blocked bytesTarget = new Blocked(); bytesTarget.release.countDown();
        camera = camera(bytesTarget); int admitted = 0;
        CaptureRequest large = request(128 * 1024);
        while (admitted < maximum) {
            try { submit(camera, large); ++admitted; }
            catch (ServiceSpecificException e) {
                require(e.errorCode == ICameraService.ERROR_INVALID_OPERATION, "native-byte limit error");
                break;
            }
        }
        require(admitted > 0 && admitted < maximum, "actual native byte capacity limits before frame count");
        require(number(get(camera, "mCallbackQueue"), "mBytes") <= budget, "native byte accounting bounded");
        try { submit(camera, request(512 * 1024)); throw new AssertionError("oversized native metadata admitted"); }
        catch (ServiceSpecificException e) {
            require(e.errorCode == ICameraService.ERROR_INVALID_OPERATION, "oversized metadata rejected");
        }
        call(camera, "disconnect", new Class<?>[0]); retired();
        require(bytesTarget.terminals.get() == admitted, "large admitted requests receive terminal errors");
        System.out.println("CameraCallbackAdmissionTest: real native metadata, bounded requests/callbacks, "
                + "ordered flush and nonjoining close passed; checks=" + checks + ", large admitted=" + admitted);
    }
}
