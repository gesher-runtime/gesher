/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.hardware.camera2.ICameraDeviceCallbacks;

import java.lang.reflect.Array;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.ArrayDeque;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Exercises the actual service's request-selection lock, without a camera or native capture. */
public final class CameraRequestRaceTest {
    private static Field field(Class<?> type, String name) throws Exception {
        Field field = type.getDeclaredField(name);
        field.setAccessible(true);
        return field;
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    /** Arrange a cancellation already waiting for the monitor when next checks its queue. */
    private static final class ContendedQueue extends ArrayDeque<Object> {
        CountDownLatch checked;
        Thread changer;

        @Override
        public boolean isEmpty() {
            CountDownLatch gate = checked;
            if (gate != null) {
                checked = null;
                gate.countDown();
                long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
                while (changer.getState() != Thread.State.BLOCKED) {
                    require(changer.isAlive() && System.nanoTime() < deadline,
                            "cancel/flush did not contend for the selection lock");
                    Thread.yield();
                }
            }
            return super.isEmpty();
        }
    }

    public static void main(String[] args) throws Exception {
        Class<?> cameraClass = Class.forName("com.gesher.server.CameraDeviceUser");
        Constructor<?> cameraConstructor = cameraClass.getDeclaredConstructors()[0];
        cameraConstructor.setAccessible(true);
        Object camera = cameraConstructor.newInstance(null, "request-race", null,
                new ICameraDeviceCallbacks.Default());
        Field repeating = field(cameraClass, "mRepeating");
        Field repeatingId = field(cameraClass, "mRepeatingId");
        Field lastRepeating = field(cameraClass, "mRepeatingLastFrame");
        Field nextNumber = field(cameraClass, "mNextFrameNumber");
        Object lock = field(cameraClass, "mLock").get(camera);
        ContendedQueue queue = new ContendedQueue();
        field(cameraClass, "mQueue").set(camera, queue);
        Class<?> requestClass = repeating.getType().getComponentType();
        Class<?> unsafeClass = Class.forName("sun.misc.Unsafe");
        Object unsafe = null;
        for (Field candidate : unsafeClass.getDeclaredFields()) {
            if (candidate.getType() == unsafeClass && Modifier.isStatic(candidate.getModifiers())) {
                candidate.setAccessible(true);
                unsafe = candidate.get(null);
                break;
            }
        }
        require(unsafe != null, "Unsafe singleton unavailable for request fixture allocation");
        Object request = unsafeClass.getMethod("allocateInstance", Class.class)
                .invoke(unsafe, requestClass);
        Object requests = Array.newInstance(requestClass, 1);
        Array.set(requests, 0, request);
        Method next = cameraClass.getDeclaredMethod("next");
        next.setAccessible(true);
        Method cancel = cameraClass.getMethod("cancelRequest", int.class);
        Method flush = cameraClass.getMethod("flush");
        AtomicReference<Throwable> error = new AtomicReference<>();
        long expected = 0;
        for (int round = 0; round < 1000; ++round) {
            boolean useCancel = (round & 1) == 0;
            CountDownLatch checked = new CountDownLatch(1);
            long[] cancelledFrame = {-2};
            Thread changer = new Thread(() -> {
                try {
                    require(checked.await(5, TimeUnit.SECONDS), "selection did not check its queue");
                    cancelledFrame[0] = (Long) (useCancel ? cancel.invoke(camera, 7)
                            : flush.invoke(camera));
                } catch (Throwable failure) {
                    error.set(failure);
                }
            }, useCancel ? "camera-cancel" : "camera-flush");
            synchronized (lock) {
                repeating.set(camera, requests);
                repeatingId.setInt(camera, 7);
                lastRepeating.setLong(camera, -1);
                queue.changer = changer;
                queue.checked = checked;
            }
            changer.start();
            Object frame = next.invoke(camera);
            changer.join(5000);
            require(!changer.isAlive(), "cancel/flush was not released");
            if (error.get() != null) throw new AssertionError("cancel/flush failed", error.get());
            long number = field(frame.getClass(), "frameNumber").getLong(frame);
            require(number == expected++, "selected frame number was lost or repeated");
            require(cancelledFrame[0] == number, "cancel/flush missed the selected in-flight frame");
            require(repeating.get(camera) == null, "cancel/flush retained a repeating request");
        }

        // A queued single capture still precedes a repeating request and keeps its assigned number.
        Class<?> frameClass = Class.forName("com.gesher.server.CameraDeviceUser$Frame");
        Constructor<?> frameConstructor = frameClass.getDeclaredConstructor(requestClass,
                int.class, long.class);
        frameConstructor.setAccessible(true);
        Object queued = frameConstructor.newInstance(request, 0, 12345L);
        synchronized (lock) {
            repeating.set(camera, requests);
            queue.add(queued);
        }
        require(next.invoke(camera) == queued, "queued capture did not precede repeating capture");
        require(nextNumber.getLong(camera) == expected, "queued capture renumbered repeating frames");
        field(cameraClass, "mDisconnected").setBoolean(camera, true);
        require(next.invoke(camera) == null, "disconnected camera selected a request");
        System.out.println("CameraRequestRaceTest: 1000 contended cancel/flush selections, "
                + "ordered frame numbers, queued priority and disconnected stop passed");
    }
}
