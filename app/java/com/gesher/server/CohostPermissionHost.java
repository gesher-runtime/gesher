/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.Manifest;
import android.app.AppOpsManager;
import android.content.pm.PackageManager;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

/** Permission checks and host consent for one admitted standalone code and live process. */
final class CohostPermissionHost implements AutoCloseable {
    static final int CHECK = 1;
    static final int REQUEST = 2;
    static final int FLAGS = 3;
    static final int CAN_REQUEST = 4;
    static final int APP_OP_MODE = 5;

    private final PermissionPolicy.ScopedOwner mOwner;
    private final ProtectedCameraPolicy mCamera;
    private final PermissionRequests mRequests;
    private final AppOpsService mAppOps;
    private volatile long mLease;
    private boolean mCancelled;
    // Clipboard transport serializes its requests and joins before close().
    private boolean mClipboardPublisher;
    private int mClipboardMetadataSequence;
    private CohostClipboard.Metadata mClipboardMetadata;

    CohostPermissionHost(PackageManagerService pm, PermissionRequests requests, String apk)
            throws Exception {
        mRequests = requests;
        mCamera = new ProtectedCameraPolicy(pm);
        mAppOps = pm.appOps();
        mOwner = pm.permissions().openScopedOwner(apk, this::isLive);
    }

    ApkPackage code() { return mOwner.code(); }
    Zygote.StandaloneInput input() { return mOwner.input(); }

    int runApplication(String runtimeOptions, String dataDir, List<String> options)
            throws Exception {
        return runApplication(runtimeOptions, dataDir, options, null);
    }

    int runApplication(String runtimeOptions, String dataDir, List<String> options, int[] streams)
            throws Exception {
        ApkPackage code = code();
        if (dataDir != null) code.setDataDir(dataDir);
        // The layout journal belongs to the host. Only the two actual data
        // trees take part in the private storage broker.
        code.prepareDataDirectories();
        AppSandbox.prepareStandaloneRuntime();
        String profile = "Gesher.Cohost." + UUID.randomUUID();
        if (Zygote.nativeEnsureAppContainer(profile, code.packageName) != 0) {
            throw new IllegalStateException("Cannot create the standalone AppContainer");
        }
        try {
            if (Zygote.nativeGrantPropertyAreaRead(profile) != 0) {
                throw new IllegalStateException("Cannot read the host system properties");
            }
            List<String> capabilities = new ArrayList<>(List.of(AppSandbox.RUNTIME_FILES));
            if (ConnectivityService.requests(code, ConnectivityService.INTERNET)) {
                capabilities.add(AppSandbox.INTERNET_CLIENT);
                capabilities.add(AppSandbox.PRIVATE_NETWORK);
            }
            List<String> arguments = new ArrayList<>(options);
            String buildProp = null;
            String frameworkRes = null;
            for (int i = 0; i < arguments.size(); ++i) {
                switch (arguments.get(i)) {
                    case "--build-prop", "--framework-res" -> {
                        String option = arguments.get(i);
                        if (++i == arguments.size()) throw new IllegalArgumentException("Missing " + option + " file");
                        String selected = new java.io.File(arguments.get(i)).getAbsolutePath();
                        arguments.set(i, selected);
                        if ("--build-prop".equals(option)) buildProp = Zygote.windowsPath(selected);
                        else frameworkRes = Zygote.windowsPath(selected);
                    }
                    case "--display", "--activity" -> i++;
                    case "--ez" -> i += 2;
                    default -> { }
                }
            }
            if (dataDir != null) {
                arguments.add("--data-dir");
                arguments.add(code.credentialProtectedDataDir());
            }
            arguments.add(code.apkPath);
            String command = Zygote.commandLine(runtimeOptions,
                    com.gesher.app.Main.class.getName(), arguments);
            return input().runCohost(command, profile, code.packageName,
                    capabilities.toArray(String[]::new), code.uid(), this,
                    Zygote.windowsPath(code.credentialProtectedDataDir()),
                    Zygote.windowsPath(code.deviceProtectedDataDir()), streams, buildProp, frameworkRes);
        } finally {
            Zygote.nativeDeleteAppContainer(profile);
        }
    }

    // The native launcher binds only the kernel process it actually created,
    // after admission and before ResumeThread. No guest supplies this value.
    private synchronized void bindNativeLease(long lease) {
        if (lease <= 0 || mLease != 0) throw new IllegalStateException("Already bound");
        mLease = lease;
        if (mCancelled) {
            Zygote.nativeCancelCohost(lease);
            throw new SecurityException("The standalone launcher already disconnected");
        }
    }

    private boolean isLive() {
        long lease = mLease;
        return lease != 0 && Zygote.nativeCohostPermissionLive(lease);
    }

    /** Invoked only by the pinned Windows transport. */
    private int[] dispatch(int operation, String[] names) throws Exception {
        if (!mOwner.isCurrent()) throw new SecurityException("Standalone permission owner retired");
        switch (operation) {
            case CHECK: return new int[] {mOwner.check(names[0])};
            case FLAGS: return new int[] {mOwner.flags(names[0])};
            case CAN_REQUEST: return new int[] {mOwner.canRequest(names[0]) ? 1 : 0};
            case APP_OP_MODE: return new int[] {mOwner.mode(names[0])};
            case REQUEST: {
                CompletableFuture<int[]> reply = new CompletableFuture<>();
                mRequests.requestScoped(mOwner, names, intent -> reply.complete(intent.getIntArrayExtra(
                        PackageManager.EXTRA_REQUEST_PERMISSIONS_RESULTS)));
                while (mOwner.isCurrent()) {
                    try {
                        return reply.get(100, TimeUnit.MILLISECONDS);
                    } catch (TimeoutException pending) {
                        // PermissionRequests keeps the same native default-No
                        // consent policy as an installed app.
                    }
                }
                throw new SecurityException("Standalone permission owner retired");
            }
            default: throw new IllegalArgumentException("Invalid standalone permission operation");
        }
    }

    private boolean isForeground() {
        long[] foreground = WindowsHost.foregroundProcess();
        return foreground != null && foreground.length == 2
                && Zygote.nativeCohostMatchesProcess(mLease, foreground[0], foreground[1]);
    }

    /** The private capture server invokes this for the pinned caller only. */
    private boolean dispatchCamera(int kind, long session) {
        return mCamera.scoped(kind, session, mOwner,
                () -> WindowsHost.sessionLockState() == 0 && isForeground()
                        ? AppOpsManager.UID_STATE_TOP : -1);
    }

    private boolean clipboardReadAllowed(boolean foreground, String tag, boolean checkOnly) {
        return mOwner.isCurrent() && (foreground
                || mOwner.check(Manifest.permission.READ_CLIPBOARD_IN_BACKGROUND)
                    == PackageManager.PERMISSION_GRANTED
                || ClipboardService.defaultIme(mOwner.packageName()))
                && mAppOps.scopedClipboardAllowed(mOwner, AppOpsManager.OP_READ_CLIPBOARD,
                        foreground, tag, checkOnly);
    }

    private boolean canReadClipboard(String tag, boolean checkOnly) {
        return WindowsHost.sessionLockState() == 0
                && clipboardReadAllowed(isForeground(), tag, checkOnly);
    }

    private ClipboardService.Snapshot readNativeClipboard(int sequence) {
        byte[] image = WindowsHost.clipboardImage();
        if (image != null && (image.length == 0 || image.length > ClipboardService.MAX_IMAGE_BYTES)) {
            image = null;
        }
        ClipboardService.Snapshot data = new ClipboardService.Snapshot(sequence,
                WindowsHost.clipboardText(), WindowsHost.clipboardHtml(), image);
        return WindowsHost.clipboardSequence() == sequence ? data : null;
    }

    private byte[] readClipboard(int cachedSequence, String tag) {
        boolean foreground = isForeground();
        if (WindowsHost.sessionLockState() != 0 || !clipboardReadAllowed(foreground, tag, false)
                || !mAppOps.noteScopedClipboard(mOwner, AppOpsManager.OP_READ_CLIPBOARD, foreground, tag)) {
            return null;
        }
        int sequence = WindowsHost.clipboardSequence();
        if (sequence == 0) return null;
        if (sequence == cachedSequence) {
            return canReadClipboard(tag, false) ? CohostClipboard.receipt(sequence) : null;
        }
        ClipboardService.Snapshot data = readNativeClipboard(sequence);
        if (data == null) return null;
        byte[] encoded = CohostClipboard.encode(data);
        return canReadClipboard(tag, false) ? encoded : null;
    }

    private byte[] clipboardMetadata(int cachedSequence, String tag) {
        if (!canReadClipboard(tag, true)) return null;
        int sequence = WindowsHost.clipboardSequence();
        if (sequence == 0) return null;
        if (sequence == cachedSequence) {
            return canReadClipboard(tag, true) ? new CohostClipboard.Metadata(sequence, 0, 0).encode() : null;
        }
        if (sequence != mClipboardMetadataSequence) {
            ClipboardService.Snapshot clip = readNativeClipboard(sequence);
            if (clip == null) return null;
            String text = clip.text(), html = clip.html();
            int formats = 0;
            if (text != null) formats |= CohostClipboard.PLAIN_PRESENT;
            if (html != null) formats |= CohostClipboard.HTML_PRESENT;
            if (clip.image() != null) formats |= CohostClipboard.IMAGE_PRESENT;
            if (text == null && html != null && clip.image() == null) {
                text = android.text.Html.fromHtml(html, android.text.Html.FROM_HTML_MODE_LEGACY).toString();
            }
            if (text != null && !text.isEmpty()) formats |= CohostClipboard.NONEMPTY_TEXT;
            mClipboardMetadata = new CohostClipboard.Metadata(sequence, formats, System.currentTimeMillis());
            mClipboardMetadataSequence = sequence;
        }
        return canReadClipboard(tag, true) ? mClipboardMetadata.encode() : null;
    }

    /** The endpoint is tied to the exact APK and process. No caller identity comes over the wire. */
    private byte[] dispatchClipboard(int operation, byte[] input) {
        if (!mOwner.isCurrent() || !WindowsHost.available()) return null;
        ByteBuffer frame = ByteBuffer.wrap(input);
        String tag = CohostClipboard.readAttribution(frame);
        int expected = switch (operation) {
            case CohostClipboard.STATE, CohostClipboard.CLEAR, CohostClipboard.CALLBACK -> 0;
            case CohostClipboard.METADATA, CohostClipboard.READ -> Integer.BYTES;
            default -> frame.remaining();
        };
        if (frame.remaining() != expected) return null;
        switch (operation) {
            case CohostClipboard.STATE: {
                boolean foreground = isForeground();
                int policy = clipboardReadAllowed(foreground, tag, true) ? CohostClipboard.CAN_READ : 0;
                if (mAppOps.scopedClipboardAllowed(mOwner, AppOpsManager.OP_WRITE_CLIPBOARD,
                        foreground, tag, true)) policy |= CohostClipboard.CAN_WRITE;
                int sequence = (policy & CohostClipboard.CAN_READ) != 0 ? WindowsHost.clipboardSequence() : 0;
                return new CohostClipboard.State(sequence, WindowsHost.sessionLockState(), policy).encode();
            }
            case CohostClipboard.METADATA:
                return clipboardMetadata(frame.getInt(), tag);
            case CohostClipboard.READ:
                return readClipboard(frame.getInt(), tag);
            case CohostClipboard.CALLBACK: {
                boolean foreground = isForeground();
                return clipboardReadAllowed(foreground, tag, false)
                        && mAppOps.noteScopedClipboard(mOwner, AppOpsManager.OP_READ_CLIPBOARD, foreground, tag)
                        ? CohostClipboard.receipt(1) : null;
            }
            case CohostClipboard.WRITE:
            case CohostClipboard.CLEAR: {
                if (!mAppOps.scopedClipboardAllowed(mOwner, AppOpsManager.OP_WRITE_CLIPBOARD,
                        isForeground(), tag, false)) return null;
                ClipboardService.Snapshot contents = operation == CohostClipboard.WRITE
                        ? CohostClipboard.decode(frame) : null;
                if (operation == CohostClipboard.WRITE && (contents == null || contents.sequence() != 0)) {
                    return null;
                }
                if (!mClipboardPublisher) {
                    if (!WindowsHost.retainClipboardPublisher()) return null;
                    mClipboardPublisher = true;
                }
                if (!mAppOps.noteScopedClipboard(mOwner, AppOpsManager.OP_WRITE_CLIPBOARD,
                        isForeground(), tag)) return null;
                int sequence = contents == null ? WindowsHost.clearClipboardSequence()
                        : WindowsHost.setClipboardSequence(contents.text(), contents.html(), contents.image());
                return CohostClipboard.receipt(sequence);
            }
            default: return null;
        }
    }

    @Override public void close() {
        if (mClipboardPublisher) {
            mClipboardPublisher = false;
            WindowsHost.releaseClipboardPublisher();
        }
        mOwner.close();
    }

    synchronized void cancel() {
        mCancelled = true;
        long lease = mLease;
        if (lease > 0) Zygote.nativeCancelCohost(lease);
    }
}
