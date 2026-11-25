/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

/**
 * {@link PasskeyBackend} over Windows' webauthn.dll (docs/passkeys.md, ADR 0093;
 * compat/devices/webauthn_jni.cc and webauthn_win.cc). Every result is Windows' own: the
 * dialog is Windows', owned by the requesting application's foreground window, and a
 * credential or an assertion is returned only for Windows' {@code S_OK}.
 *
 * <p>No JNI callbacks: the calling thread blocks in the native ceremony.
 */
final class WindowsWebAuthn implements PasskeyBackend {
    private WindowsWebAuthn() {
    }

    /** The backend, or null when the natives are not in this runtime (host tests, old builds). */
    static WindowsWebAuthn create() {
        try {
            nativeApiVersion();
            return new WindowsWebAuthn();
        } catch (UnsatisfiedLinkError unavailable) {
            return null;
        }
    }

    @Override
    public int apiVersion() {
        return nativeApiVersion();
    }

    @Override
    public int platformAuthenticator() {
        int answer = nativePlatformAuthenticator();
        return answer == 0 || answer == 1 ? answer : -1;
    }

    @Override
    public byte[][] platformCredentials(String rpId) {
        return nativePlatformCredentials(rpId);
    }

    @Override
    public Cancellation newCancellation() {
        byte[] id = nativeNewCancellation();
        return id == null ? null : new Id(id);
    }

    /** A WebAuthNGetCancellationId GUID. */
    static final class Id implements Cancellation {
        private static final long CANCEL_RETRY_MS = 100;
        final byte[] mGuid;
        private final Runnable mNativeCancel;
        private boolean mStarted;
        private boolean mCanceled;
        private boolean mComplete;
        private Thread mCancelWorker;

        Id(byte[] guid) {
            this(guid, () -> nativeCancel(guid));
        }

        /** Native cancellation boundary, supplied by the host tests without a dialog. */
        Id(byte[] guid, Runnable nativeCancel) {
            mGuid = guid;
            mNativeCancel = nativeCancel;
        }

        synchronized boolean begin() {
            if (mCanceled || mStarted || mComplete) return false;
            mStarted = true;
            return true;
        }

        @Override
        public synchronized void cancel() {
            if (mCanceled || mComplete) return;
            mCanceled = true;
            if (!mStarted) return;
            // WebAuthNCancelCurrentOperation does nothing before the synchronous operation
            // starts. Reissue across the JNI/native-start handoff until that call returns.
            mCancelWorker = new Thread(this::cancelUntilComplete, "webauthn-cancel");
            mCancelWorker.setDaemon(true);
            mCancelWorker.start();
        }

        private synchronized void cancelUntilComplete() {
            while (!mComplete) {
                mNativeCancel.run();
                if (mComplete) return;
                try {
                    wait(CANCEL_RETRY_MS);
                } catch (InterruptedException interrupted) {
                    // Completion owns this worker's lifetime; an interrupt must not reopen
                    // the handoff gap or turn a canceled request back into a live one.
                }
            }
        }

        void finish() {
            Thread worker;
            synchronized (this) {
                mComplete = true;
                notifyAll();
                worker = mCancelWorker;
            }
            if (worker != null) {
                // No native cancel can be in progress once the lock above was acquired,
                // and mComplete forbids another. The worker only has to leave its loop.
                boolean interrupted = false;
                while (worker.isAlive()) {
                    try {
                        worker.join();
                    } catch (InterruptedException again) {
                        interrupted = true;
                    }
                }
                if (interrupted) Thread.currentThread().interrupt();
                synchronized (this) {
                    mCancelWorker = null;
                }
            }
        }
    }

    /** Synchronous native-call boundary, shared by both ceremonies and the host contracts. */
    static int call(Cancellation cancellation, java.util.function.IntSupplier nativeCall) {
        Id id = cancellation instanceof Id ? (Id) cancellation : null;
        // A failed WebAuthNGetCancellationId must not open UI that app cancellation
        // or Binder death cannot close. This backend always requires its native GUID.
        if (id == null) return 0x80004005;  // E_FAIL, no cancellable native operation
        if (!id.begin()) return 0x800704C7;  // ERROR_CANCELLED, no native call
        try {
            return nativeCall.getAsInt();
        } finally {
            id.finish();
        }
    }

    private static byte[] guid(Cancellation cancellation) {
        return cancellation instanceof Id ? ((Id) cancellation).mGuid : null;
    }

    @Override
    public Attestation makeCredential(MakeCredential request, Cancellation cancellation) {
        byte[][] excludeIds = new byte[request.exclude.length][];
        int[] excludeTransports = new int[request.exclude.length];
        for (int i = 0; i < request.exclude.length; i++) {
            excludeIds[i] = request.exclude[i].id;
            excludeTransports[i] = request.exclude[i].transports;
        }
        Object[] out = new Object[4];
        int[] info = {0, 0, -1, -1};
        Attestation attestation = new Attestation();
        attestation.hresult = call(cancellation, () -> nativeMakeCredential(
                request.pid, request.rpId, request.rpName,
                request.userId, request.userName, request.userDisplayName, request.algorithms,
                request.clientDataJson, request.timeoutMs, excludeIds, excludeTransports,
                request.attachment, request.residentKey, request.userVerification,
                request.attestation, guid(cancellation), out, info));
        attestation.notForeground = info[0] != 0;
        attestation.errorName = (String) out[3];
        if (attestation.hresult != S_OK) return attestation;
        attestation.credentialId = (byte[]) out[0];
        attestation.authenticatorData = (byte[]) out[1];
        attestation.attestationObject = (byte[]) out[2];
        attestation.usedTransport = info[1];
        attestation.transports = info[2];
        attestation.residentKey = info[3];
        if (attestation.credentialId == null || attestation.authenticatorData == null
                || attestation.attestationObject == null) {
            attestation.hresult = 0x8000FFFF;  // E_UNEXPECTED: the result could not be copied
        }
        return attestation;
    }

    @Override
    public Assertion getAssertion(GetAssertion request, Cancellation cancellation) {
        byte[][] allowIds = new byte[request.allow.length][];
        int[] allowTransports = new int[request.allow.length];
        for (int i = 0; i < request.allow.length; i++) {
            allowIds[i] = request.allow[i].id;
            allowTransports[i] = request.allow[i].transports;
        }
        Object[] out = new Object[5];
        int[] info = {0, 0};
        Assertion assertion = new Assertion();
        assertion.hresult = call(cancellation, () -> nativeGetAssertion(
                request.pid, request.rpId, request.clientDataJson,
                request.timeoutMs, allowIds, allowTransports, request.userVerification,
                guid(cancellation), out, info));
        assertion.notForeground = info[0] != 0;
        assertion.errorName = (String) out[4];
        if (assertion.hresult != S_OK) return assertion;
        assertion.credentialId = (byte[]) out[0];
        assertion.authenticatorData = (byte[]) out[1];
        assertion.signature = (byte[]) out[2];
        assertion.userId = (byte[]) out[3];
        assertion.usedTransport = info[1];
        return assertion;
    }

    private static native int nativeApiVersion();

    private static native int nativePlatformAuthenticator();

    private static native byte[][] nativePlatformCredentials(String rpId);

    private static native byte[] nativeNewCancellation();

    private static native int nativeCancel(byte[] cancellation);

    private static native int nativeMakeCredential(int pid, String rpId, String rpName,
            byte[] userId, String userName, String userDisplayName, int[] algorithms,
            byte[] clientDataJson, int timeoutMs, byte[][] excludeIds, int[] excludeTransports,
            int attachment, int residentKey, int userVerification, int attestation,
            byte[] cancellation, Object[] out, int[] info);

    private static native int nativeGetAssertion(int pid, String rpId, byte[] clientDataJson,
            int timeoutMs, byte[][] allowIds, int[] allowTransports, int userVerification,
            byte[] cancellation, Object[] out, int[] info);
}
