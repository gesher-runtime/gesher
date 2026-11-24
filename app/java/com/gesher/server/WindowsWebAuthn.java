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
    public Cancellation newCancellation() {
        byte[] id = nativeNewCancellation();
        return id == null ? null : new Id(id);
    }

    /** A WebAuthNGetCancellationId GUID. */
    private static final class Id implements Cancellation {
        final byte[] mGuid;

        Id(byte[] guid) {
            mGuid = guid;
        }

        @Override
        public void cancel() {
            nativeCancel(mGuid);
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
        attestation.hresult = nativeMakeCredential(request.pid, request.rpId, request.rpName,
                request.userId, request.userName, request.userDisplayName, request.algorithms,
                request.clientDataJson, request.timeoutMs, excludeIds, excludeTransports,
                request.attachment, request.residentKey, request.userVerification,
                request.attestation, guid(cancellation), out, info);
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
        assertion.hresult = nativeGetAssertion(request.pid, request.rpId, request.clientDataJson,
                request.timeoutMs, allowIds, allowTransports, request.userVerification,
                guid(cancellation), out, info);
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
