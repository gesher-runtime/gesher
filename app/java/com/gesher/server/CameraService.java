/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.Manifest;
import android.app.ActivityThread;
import android.content.AttributionSourceState;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.CameraCompatibilityInfo;
import android.graphics.Rect;
import android.hardware.CameraStatus;
import android.hardware.ICameraService;
import android.hardware.ICameraServiceListener;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraMetadata;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.ICameraDeviceCallbacks;
import android.hardware.camera2.ICameraDeviceUser;
import android.hardware.camera2.impl.CameraMetadataNative;
import android.hardware.camera2.params.SessionConfiguration;
import android.hardware.camera2.params.StreamConfiguration;
import android.hardware.camera2.params.StreamConfigurationDuration;
import android.hardware.camera2.utils.CameraIdAndSessionConfiguration;
import android.hardware.camera2.utils.ConcurrentCameraIdCombination;
import android.os.IBinder;
import android.os.ServiceManager;
import android.os.ServiceSpecificException;
import android.util.Range;
import android.util.Rational;
import android.util.Size;
import android.util.SizeF;

import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * {@code media.camera}: cameraserver's ICameraService on Windows (docs/devices.md, ADR
 * 0043), over {@link WindowsCamera}'s devices (Media Foundation's, and GESHER_CAMERA_TEST's
 * test camera). It runs in the app's own process: a capture request's Surfaces are this
 * process's objects (Gesher's ImageReader natives, or native windows), which no binder
 * transaction could carry. Camera ids are "0", "1"... in Windows' order.
 *
 * <p>Characteristics describe an EXTERNAL-level camera (as Android's USB cameras): fixed
 * focus, auto exposure and white balance only, outputs YUV_420_888, JPEG and PRIVATE at the
 * device's sizes (frames are scaled for other sizes). Opening needs the CAMERA permission in
 * the app's manifest. No torch, no reprocessing, no offline sessions, no extensions.
 */
public final class CameraService extends ICameraService.Default {
    static final String TAG = "camera";
    static final String SERVICE = "media.camera";

    // HAL pixel formats (system/graphics.h), as the metadata names them.
    static final int HAL_BLOB = 0x21;
    static final int HAL_PRIVATE = 0x22;
    static final int HAL_YCBCR_420_888 = 0x23;
    static final long FRAME_DURATION_NS = 33_333_333L;

    private static volatile CameraService sInstance;

    private volatile List<WindowsCamera.Device> mDevices;
    private final Map<String, CameraDeviceUser> mOpen = new HashMap<>();

    private CameraService() {
    }

    /** The devices, found on first use (not when the process starts). */
    private List<WindowsCamera.Device> devices() {
        List<WindowsCamera.Device> devices = mDevices;
        if (devices == null) {
            synchronized (this) {
                if (mDevices == null) {
                    mDevices = WindowsCamera.devices();
                    log(WindowsCamera.status() + "; camera ids " + ids(mDevices));
                }
                devices = mDevices;
            }
        }
        return devices;
    }

    static void log(String message) {
        System.out.println(TAG + ": " + message);
        System.out.flush();
    }

    /** This process's camera service (created on first use). */
    public static synchronized CameraService get() {
        if (sInstance == null) sInstance = new CameraService();
        return sInstance;
    }

    /**
     * Puts {@code media.camera} into this process's ServiceManager cache, where
     * CameraManager looks first (the app process of the Gesher device, and the
     * single-process mode after its services are installed).
     */
    public static void installInProcess() {
        try {
            IBinder binder = StandIn.of(ICameraService.class, get()).asBinder();
            Field f = ServiceManager.class.getDeclaredField("sCache");
            f.setAccessible(true);
            @SuppressWarnings("unchecked")
            Map<String, IBinder> cache = (Map<String, IBinder>) f.get(null);
            synchronized (cache) {
                cache.put(SERVICE, binder);
            }
        } catch (ReflectiveOperationException | RuntimeException e) {
            log("no camera service in this process: " + e);
        }
    }

    private static List<String> ids(List<WindowsCamera.Device> devices) {
        List<String> ids = new ArrayList<>();
        for (int i = 0; i < devices.size(); i++) ids.add(Integer.toString(i));
        return ids;
    }

    WindowsCamera.Device device(String cameraId) {
        try {
            int i = Integer.parseInt(cameraId);
            List<WindowsCamera.Device> devices = devices();
            if (i >= 0 && i < devices.size()) return devices.get(i);
        } catch (NumberFormatException e) {
            // not ours
        }
        throw new ServiceSpecificException(ERROR_ILLEGAL_ARGUMENT,
                "no camera device with id " + cameraId);
    }

    // --- the device list -------------------------------------------------------------------

    @Override
    public CameraStatus[] addListener(ICameraServiceListener listener) {
        CameraStatus[] statuses = new CameraStatus[devices().size()];
        for (int i = 0; i < statuses.length; i++) {
            CameraStatus s = new CameraStatus();
            s.cameraId = Integer.toString(i);
            s.status = ICameraServiceListener.STATUS_PRESENT;
            s.unavailablePhysicalCameras = new String[0];
            s.clientPackage = "";
            s.deviceId = 0;
            statuses[i] = s;
        }
        return statuses;
    }

    @Override
    public void removeListener(ICameraServiceListener listener) {
    }

    @Override
    public int getNumberOfCameras(int type, AttributionSourceState clientAttribution,
            int devicePolicy) {
        return devices().size();
    }

    @Override
    public ConcurrentCameraIdCombination[] getConcurrentCameraIds() {
        return new ConcurrentCameraIdCombination[0];
    }

    @Override
    public boolean isConcurrentSessionConfigurationSupported(
            CameraIdAndSessionConfiguration[] sessions, int targetSdkVersion,
            AttributionSourceState clientAttribution, int devicePolicy) {
        return false;
    }

    @Override
    public boolean isHiddenPhysicalCamera(String cameraId) {
        return false;
    }

    @Override
    public android.hardware.camera2.params.VendorTagDescriptor getCameraVendorTagDescriptor() {
        return null;
    }

    @Override
    public android.hardware.camera2.params.VendorTagDescriptorCache getCameraVendorTagCache() {
        return null;
    }

    @Override
    public void notifySystemEvent(int eventId, int[] args) {
    }

    @Override
    public void notifyDisplayConfigurationChange() {
    }

    @Override
    public void notifyDeviceStateChange(long newState) {
    }

    @Override
    public void setTorchMode(String cameraId, boolean enabled, IBinder clientBinder,
            AttributionSourceState clientAttribution, int devicePolicy) {
        throw new ServiceSpecificException(ERROR_ILLEGAL_ARGUMENT,
                "camera " + cameraId + " has no flash unit");
    }

    // --- characteristics -------------------------------------------------------------------

    @Override
    public CameraMetadataNative getCameraCharacteristics(String cameraId, int targetSdkVersion,
            CameraCompatibilityInfo compatInfo, AttributionSourceState clientAttribution,
            int devicePolicy) {
        return characteristics(device(cameraId));
    }

    @Override
    public CameraMetadataNative getSessionCharacteristics(String cameraId, int targetSdkVersion,
            CameraCompatibilityInfo compatInfo, SessionConfiguration sessionConfiguration,
            AttributionSourceState clientAttribution, int devicePolicy) {
        return characteristics(device(cameraId));
    }

    @Override
    public boolean isSessionConfigurationWithParametersSupported(String cameraId,
            int targetSdkVersion, SessionConfiguration sessionConfiguration,
            AttributionSourceState clientAttribution, int devicePolicy) {
        return true;
    }

    @Override
    public CameraMetadataNative createDefaultRequest(String cameraId, int templateId,
            AttributionSourceState clientAttribution, int devicePolicy) {
        return defaultRequest(device(cameraId), templateId);
    }

    static CameraMetadataNative characteristics(WindowsCamera.Device d) {
        CameraMetadataNative m = new CameraMetadataNative();
        Size largest = d.sizes.get(0);
        List<StreamConfiguration> configs = new ArrayList<>();
        List<StreamConfigurationDuration> durations = new ArrayList<>();
        List<StreamConfigurationDuration> stalls = new ArrayList<>();
        for (int format : new int[] {HAL_YCBCR_420_888, HAL_PRIVATE, HAL_BLOB}) {
            for (Size s : d.sizes) {
                configs.add(new StreamConfiguration(format, s.getWidth(), s.getHeight(), false));
                durations.add(new StreamConfigurationDuration(format, s.getWidth(),
                        s.getHeight(), FRAME_DURATION_NS));
                stalls.add(new StreamConfigurationDuration(format, s.getWidth(), s.getHeight(),
                        format == HAL_BLOB ? FRAME_DURATION_NS : 0));
            }
        }
        m.set(CameraCharacteristics.SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                configs.toArray(new StreamConfiguration[0]));
        m.set(CameraCharacteristics.SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
                durations.toArray(new StreamConfigurationDuration[0]));
        m.set(CameraCharacteristics.SCALER_AVAILABLE_STALL_DURATIONS,
                stalls.toArray(new StreamConfigurationDuration[0]));
        m.set(CameraCharacteristics.LENS_FACING, d.test ? CameraMetadata.LENS_FACING_BACK
                : CameraMetadata.LENS_FACING_EXTERNAL);
        m.set(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL,
                CameraMetadata.INFO_SUPPORTED_HARDWARE_LEVEL_EXTERNAL);
        m.set(CameraCharacteristics.SENSOR_ORIENTATION, 0);
        m.set(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES,
                new int[] {CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE});
        m.set(CameraCharacteristics.REQUEST_PARTIAL_RESULT_COUNT, 1);
        m.set(CameraCharacteristics.REQUEST_PIPELINE_MAX_DEPTH, (byte) 4);
        m.set(CameraCharacteristics.REQUEST_MAX_NUM_OUTPUT_STREAMS, new int[] {0, 3, 1});
        m.set(CameraCharacteristics.REQUEST_MAX_NUM_INPUT_STREAMS, 0);
        m.set(CameraCharacteristics.SYNC_MAX_LATENCY, CameraMetadata.SYNC_MAX_LATENCY_UNKNOWN);
        m.set(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE,
                new Rect(0, 0, largest.getWidth(), largest.getHeight()));
        m.set(CameraCharacteristics.SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE,
                new Rect(0, 0, largest.getWidth(), largest.getHeight()));
        m.set(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE, largest);
        m.set(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE, new SizeF(3.2f, 2.4f));
        m.set(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE,
                CameraMetadata.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME);
        m.set(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
                new Range[] {Range.create(15, 30), Range.create(30, 30)});
        m.set(CameraCharacteristics.CONTROL_AE_AVAILABLE_MODES,
                new int[] {CameraMetadata.CONTROL_AE_MODE_ON});
        m.set(CameraCharacteristics.CONTROL_AE_AVAILABLE_ANTIBANDING_MODES,
                new int[] {CameraMetadata.CONTROL_AE_ANTIBANDING_MODE_AUTO});
        m.set(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE, Range.create(0, 0));
        m.set(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP, new Rational(1, 1));
        m.set(CameraCharacteristics.CONTROL_AE_LOCK_AVAILABLE, false);
        m.set(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES,
                new int[] {CameraMetadata.CONTROL_AF_MODE_OFF});
        m.set(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES,
                new int[] {CameraMetadata.CONTROL_AWB_MODE_AUTO});
        m.set(CameraCharacteristics.CONTROL_AWB_LOCK_AVAILABLE, false);
        m.set(CameraCharacteristics.CONTROL_AVAILABLE_MODES,
                new int[] {CameraMetadata.CONTROL_MODE_AUTO});
        m.set(CameraCharacteristics.CONTROL_AVAILABLE_SCENE_MODES,
                new int[] {CameraMetadata.CONTROL_SCENE_MODE_DISABLED});
        m.set(CameraCharacteristics.CONTROL_AVAILABLE_EFFECTS,
                new int[] {CameraMetadata.CONTROL_EFFECT_MODE_OFF});
        m.set(CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
                new int[] {CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_OFF});
        m.set(CameraCharacteristics.CONTROL_MAX_REGIONS, new int[] {0, 0, 0});
        m.set(CameraCharacteristics.FLASH_INFO_AVAILABLE, false);
        m.set(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS, new float[] {3.5f});
        m.set(CameraCharacteristics.LENS_INFO_AVAILABLE_APERTURES, new float[] {2.0f});
        m.set(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE, 0f);
        m.set(CameraCharacteristics.LENS_INFO_FOCUS_DISTANCE_CALIBRATION,
                CameraMetadata.LENS_INFO_FOCUS_DISTANCE_CALIBRATION_UNCALIBRATED);
        m.set(CameraCharacteristics.JPEG_AVAILABLE_THUMBNAIL_SIZES,
                new Size[] {new Size(0, 0)});
        m.set(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, 1f);
        m.set(CameraCharacteristics.SCALER_CROPPING_TYPE,
                CameraMetadata.SCALER_CROPPING_TYPE_CENTER_ONLY);
        m.set(CameraCharacteristics.STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES,
                new int[] {CameraMetadata.STATISTICS_FACE_DETECT_MODE_OFF});
        m.set(CameraCharacteristics.STATISTICS_INFO_MAX_FACE_COUNT, 0);
        m.set(CameraCharacteristics.NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
                new int[] {CameraMetadata.NOISE_REDUCTION_MODE_OFF});
        m.set(CameraCharacteristics.EDGE_AVAILABLE_EDGE_MODES,
                new int[] {CameraMetadata.EDGE_MODE_OFF});
        m.set(CameraCharacteristics.REQUEST_AVAILABLE_CHARACTERISTICS_KEYS, tagsOf(m));
        m.set(CameraCharacteristics.REQUEST_AVAILABLE_REQUEST_KEYS,
                tagsOf(defaultRequest(d, CameraDevice_TEMPLATE_PREVIEW)));
        m.set(CameraCharacteristics.REQUEST_AVAILABLE_RESULT_KEYS,
                tagsOf(CameraDeviceUser.result(defaultRequest(d, CameraDevice_TEMPLATE_PREVIEW),
                        0)));
        m.set(CameraCharacteristics.REQUEST_AVAILABLE_SESSION_KEYS, new int[0]);
        return m;
    }

    /** CameraDevice.TEMPLATE_PREVIEW (ICameraDeviceUser's too). */
    static final int CameraDevice_TEMPLATE_PREVIEW = ICameraDeviceUser.TEMPLATE_PREVIEW;

    /** The tags of what {@code m} holds, for the available-keys lists. */
    private static int[] tagsOf(CameraMetadataNative m) {
        List<Integer> tags = new ArrayList<>();
        for (Field f : CaptureRequest.class.getFields()) {
            collect(f, m, tags);
        }
        for (Field f : CameraCharacteristics.class.getFields()) {
            collect(f, m, tags);
        }
        for (Field f : android.hardware.camera2.CaptureResult.class.getFields()) {
            collect(f, m, tags);
        }
        int[] result = new int[tags.size()];
        for (int i = 0; i < result.length; i++) result[i] = tags.get(i);
        return result;
    }

    private static void collect(Field f, CameraMetadataNative m, List<Integer> tags) {
        try {
            Object key = f.get(null);
            String name;
            if (key instanceof CaptureRequest.Key) {
                name = ((CaptureRequest.Key<?>) key).getName();
            } else if (key instanceof CameraCharacteristics.Key) {
                name = ((CameraCharacteristics.Key<?>) key).getName();
            } else if (key instanceof android.hardware.camera2.CaptureResult.Key) {
                name = ((android.hardware.camera2.CaptureResult.Key<?>) key).getName();
            } else {
                return;
            }
            int tag = CameraMetadataNative.getTag(name);
            if (!tags.contains(tag) && m.readValues(tag) != null) tags.add(tag);
        } catch (IllegalAccessException | IllegalArgumentException e) {
            // synthetic keys have no tag
        }
    }

    static CameraMetadataNative defaultRequest(WindowsCamera.Device d, int templateId) {
        if (templateId < ICameraDeviceUser.TEMPLATE_PREVIEW
                || templateId > ICameraDeviceUser.TEMPLATE_MANUAL) {
            throw new ServiceSpecificException(ERROR_ILLEGAL_ARGUMENT,
                    "unknown template " + templateId);
        }
        if (templateId == ICameraDeviceUser.TEMPLATE_MANUAL
                || templateId == ICameraDeviceUser.TEMPLATE_ZERO_SHUTTER_LAG) {
            throw new ServiceSpecificException(ERROR_ILLEGAL_ARGUMENT,
                    "template " + templateId + " is not supported by this camera");
        }
        CameraMetadataNative m = new CameraMetadataNative();
        int intent;
        switch (templateId) {
            case ICameraDeviceUser.TEMPLATE_STILL_CAPTURE:
                intent = CameraMetadata.CONTROL_CAPTURE_INTENT_STILL_CAPTURE;
                break;
            case ICameraDeviceUser.TEMPLATE_RECORD:
                intent = CameraMetadata.CONTROL_CAPTURE_INTENT_VIDEO_RECORD;
                break;
            case ICameraDeviceUser.TEMPLATE_VIDEO_SNAPSHOT:
                intent = CameraMetadata.CONTROL_CAPTURE_INTENT_VIDEO_SNAPSHOT;
                break;
            default:
                intent = CameraMetadata.CONTROL_CAPTURE_INTENT_PREVIEW;
                break;
        }
        m.set(CaptureRequest.CONTROL_CAPTURE_INTENT, intent);
        m.set(CaptureRequest.CONTROL_MODE, CameraMetadata.CONTROL_MODE_AUTO);
        m.set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_ON);
        m.set(CaptureRequest.CONTROL_AE_ANTIBANDING_MODE,
                CameraMetadata.CONTROL_AE_ANTIBANDING_MODE_AUTO);
        m.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, 0);
        m.set(CaptureRequest.CONTROL_AE_LOCK, false);
        m.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, Range.create(15, 30));
        m.set(CaptureRequest.CONTROL_AE_PRECAPTURE_TRIGGER,
                CameraMetadata.CONTROL_AE_PRECAPTURE_TRIGGER_IDLE);
        m.set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_OFF);
        m.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_IDLE);
        m.set(CaptureRequest.CONTROL_AWB_MODE, CameraMetadata.CONTROL_AWB_MODE_AUTO);
        m.set(CaptureRequest.CONTROL_AWB_LOCK, false);
        m.set(CaptureRequest.CONTROL_EFFECT_MODE, CameraMetadata.CONTROL_EFFECT_MODE_OFF);
        m.set(CaptureRequest.CONTROL_SCENE_MODE, CameraMetadata.CONTROL_SCENE_MODE_DISABLED);
        m.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE,
                CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_OFF);
        m.set(CaptureRequest.FLASH_MODE, CameraMetadata.FLASH_MODE_OFF);
        m.set(CaptureRequest.JPEG_QUALITY, (byte) 95);
        m.set(CaptureRequest.JPEG_ORIENTATION, 0);
        m.set(CaptureRequest.JPEG_THUMBNAIL_SIZE, new Size(0, 0));
        m.set(CaptureRequest.LENS_FOCAL_LENGTH, 3.5f);
        m.set(CaptureRequest.LENS_APERTURE, 2.0f);
        m.set(CaptureRequest.NOISE_REDUCTION_MODE, CameraMetadata.NOISE_REDUCTION_MODE_OFF);
        m.set(CaptureRequest.EDGE_MODE, CameraMetadata.EDGE_MODE_OFF);
        m.set(CaptureRequest.STATISTICS_FACE_DETECT_MODE,
                CameraMetadata.STATISTICS_FACE_DETECT_MODE_OFF);
        m.set(CaptureRequest.SCALER_CROP_REGION,
                new Rect(0, 0, d.sizes.get(0).getWidth(), d.sizes.get(0).getHeight()));
        return m;
    }

    // --- opening -----------------------------------------------------------------------------

    @Override
    public ICameraDeviceUser connectDevice(ICameraDeviceCallbacks callbacks, String cameraId,
            int oomScoreOffset, int targetSdkVersion, CameraCompatibilityInfo compatInfo,
            AttributionSourceState clientAttribution, int devicePolicy, boolean sharedMode) {
        WindowsCamera.Device d = device(cameraId);
        String packageName = clientAttribution != null ? clientAttribution.packageName : null;
        if (!hasCameraPermission(packageName)) {
            throw new ServiceSpecificException(ERROR_PERMISSION_DENIED, "package " + packageName
                    + " does not have " + Manifest.permission.CAMERA);
        }
        CameraDeviceUser previous;
        CameraDeviceUser user = new CameraDeviceUser(this, cameraId, d, callbacks);
        synchronized (mOpen) {
            previous = mOpen.put(cameraId, user);
        }
        if (previous != null) {
            // A new client takes the camera, as cameraserver gives it to the newest.
            previous.evicted();
        }
        log("connectDevice " + cameraId + " (" + d.name + ") for " + packageName);
        return StandIn.of(ICameraDeviceUser.class, user).asInterface();
    }

    void closed(String cameraId, CameraDeviceUser user) {
        synchronized (mOpen) {
            if (mOpen.get(cameraId) == user) mOpen.remove(cameraId);
        }
    }

    /** What the package's manifest asks for (Gesher grants that; docs/devices.md). */
    private static boolean hasCameraPermission(String packageName) {
        if (packageName == null) return false;
        try {
            PackageInfo info = ActivityThread.getPackageManager().getPackageInfo(packageName,
                    PackageManager.GET_PERMISSIONS, 0);
            if (info == null || info.requestedPermissions == null) return false;
            for (String p : info.requestedPermissions) {
                if (Manifest.permission.CAMERA.equals(p)) return true;
            }
            return false;
        } catch (android.os.RemoteException | RuntimeException e) {
            log("checking " + packageName + "'s permissions: " + e);
            return false;
        }
    }
}
