/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package android.app;

import android.content.Context;
import android.hardware.ICameraService;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CameraMetadataInfo;
import android.hardware.camera2.ICameraDeviceCallbacks;
import android.hardware.camera2.ICameraDeviceUser;
import android.hardware.camera2.impl.CaptureResultExtras;
import android.hardware.camera2.impl.PhysicalCaptureResultInfo;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.ServiceSpecificException;

import java.util.Arrays;

/** Camera2 discovery and device ownership for the native camera manager. */
final class GesherCameraManager {
    private GesherCameraManager() { }

    private static Context appContext() {
        ActivityThread thread = ActivityThread.currentActivityThread();
        if (thread == null) return null;
        Context context = thread.getApplication();
        if (context == null && thread.mBoundApplication != null
                && thread.mBoundApplication.info != null) {
            // A library can query before the Application object is published.
            context = ContextImpl.createAppContext(thread, thread.mBoundApplication.info);
        }
        return context;
    }

    private static String[] getCameraIdList() throws CameraAccessException {
        Context context = appContext();
        CameraManager manager = context == null ? null : context.getSystemService(CameraManager.class);
        return manager == null ? null : manager.getCameraIdList();
    }

    // Camera NDK opens synchronously through the Camera2 connection. Use the same
    // attributed caller as Java CameraManager so the service enforces admission.
    private static int openCamera(String id, Device device) throws RemoteException, CameraAccessException {
        Context context = appContext();
        CameraManager manager = context == null ? null : context.getSystemService(CameraManager.class);
        ICameraService service = ICameraService.Stub.asInterface(ServiceManager.checkService("media.camera"));
        if (manager == null || service == null) return ICameraService.ERROR_DISCONNECTED;
        if (!Arrays.asList(manager.getCameraIdList()).contains(id)) {
            return ICameraService.ERROR_ILLEGAL_ARGUMENT;
        }
        try {
            ICameraDeviceUser peer = service.connectDevice(device, id, 0,
                    context.getApplicationInfo().targetSdkVersion, CameraManager.getRotationOverride(context),
                    manager.getClientAttribution(true), manager.getDevicePolicyFromContext(context), false);
            if (peer == null) return ICameraService.ERROR_DISCONNECTED;
            device.attach(peer);
            return 0;
        } catch (ServiceSpecificException failure) {
            return failure.errorCode;
        }
    }

    private static native void dispatchTerminal(long device, int error);

    private static final class Device extends ICameraDeviceCallbacks.Stub
            implements IBinder.DeathRecipient {
        private ICameraDeviceUser peer;
        private long pointer;
        private boolean closed;
        private boolean dispatched;

        private synchronized void attach(ICameraDeviceUser connected) throws RemoteException {
            peer = connected;
            peer.asBinder().linkToDeath(this, 0);
        }

        private synchronized void publish(long value) {
            pointer = value;
            notifyAll();
        }

        private void close() throws RemoteException {
            ICameraDeviceUser disconnect;
            synchronized (this) {
                closed = true;
                pointer = 0;
                disconnect = peer;
                peer = null;
                notifyAll();
            }
            if (disconnect == null) return;
            try {
                disconnect.asBinder().unlinkToDeath(this, 0);
            } catch (java.util.NoSuchElementException alreadyUnlinked) {
                // An unsuccessful link or service death has no remaining recipient.
            }
            disconnect.disconnect();
        }

        private synchronized void terminal(int error) {
            if (closed || dispatched) return;
            dispatched = true;
            // Application code must leave the service callback thread before it can
            // reenter close. No worker is needed unless the device has a terminal event.
            Thread callback = new Thread(() -> deliver(error), "gesher-camera-ndk");
            callback.setDaemon(true);
            callback.start();
        }

        private synchronized void deliver(int error) {
            // A service can disconnect while open is still publishing the native handle.
            while (pointer == 0 && !closed) {
                try {
                    wait();
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    return;
                }
            }
            if (!closed) dispatchTerminal(pointer, error);
        }

        @Override public void binderDied() { terminal(5); }

        @Override public void onDeviceError(int error, CaptureResultExtras extras) {
            switch (error) {
                case ERROR_CAMERA_DISABLED:
                case ERROR_CAMERA_DISCONNECTED: terminal(0); break;
                case ERROR_CAMERA_DEVICE: terminal(4); break;
                case ERROR_CAMERA_SERVICE: terminal(5); break;
                default: break;
            }
        }

        @Override public void onDeviceIdle() { }
        @Override public void onCaptureStarted(CaptureResultExtras extras, long timestamp) { }
        @Override public void onResultReceived(CameraMetadataInfo info, CaptureResultExtras extras,
                PhysicalCaptureResultInfo[] physicalResults) { }
        @Override public void onPrepared(int streamId) { }
        @Override public void onRepeatingRequestError(long lastFrameNumber, int requestId) { }
        @Override public void onRequestQueueEmpty() { }
        @Override public void onClientSharedAccessPriorityChanged(boolean isPrimary) { }
    }
}
