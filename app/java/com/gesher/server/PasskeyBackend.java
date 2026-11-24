/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

/**
 * What {@link CredentialManagerService} needs from Windows' WebAuthn API (webauthn.dll), in
 * Windows' own values (docs/passkeys.md). The only production implementation is
 * {@link WindowsWebAuthn}; test doubles live in tests/credentials.
 *
 * <p>Both ceremonies block the calling thread while Windows shows its own dialog (Windows
 * Hello, a security key, a phone) and return Windows' result unchanged. Nothing here may
 * report a credential or an assertion that Windows did not return.
 */
public interface PasskeyBackend {
    /** {@code S_OK}. */
    int S_OK = 0;

    /** WEBAUTHN_AUTHENTICATOR_ATTACHMENT_*. */
    int ATTACHMENT_ANY = 0, ATTACHMENT_PLATFORM = 1, ATTACHMENT_CROSS_PLATFORM = 2;
    /** WEBAUTHN_USER_VERIFICATION_REQUIREMENT_*. */
    int UV_REQUIRED = 1, UV_PREFERRED = 2, UV_DISCOURAGED = 3;
    /** WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_*. */
    int ATTESTATION_NONE = 1, ATTESTATION_INDIRECT = 2, ATTESTATION_DIRECT = 3;
    /** How a discoverable credential is asked for (bRequireResidentKey, bPreferResidentKey). */
    int RESIDENT_KEY_DISCOURAGED = 0, RESIDENT_KEY_PREFERRED = 1, RESIDENT_KEY_REQUIRED = 2;
    /** WEBAUTHN_CTAP_TRANSPORT_*. */
    int TRANSPORT_USB = 0x1, TRANSPORT_NFC = 0x2, TRANSPORT_BLE = 0x4, TRANSPORT_INTERNAL = 0x10,
            TRANSPORT_HYBRID = 0x20, TRANSPORT_SMART_CARD = 0x40;

    /** {@code WebAuthNGetApiVersionNumber}; 0 when Windows has no WebAuthn API. */
    int apiVersion();

    /**
     * {@code WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable}: 1 when this PC can
     * verify the user itself (Windows Hello), 0 when it cannot, -1 when Windows did not say.
     */
    int platformAuthenticator();

    /**
     * The ids of the credentials this PC's platform authenticator holds for {@code rpId}
     * ({@code WebAuthNGetPlatformCredentialList}); null when this Windows cannot list them
     * (before API version 4, or the query failed).
     */
    byte[][] platformCredentials(String rpId);

    /**
     * A cancellation id for one ceremony ({@code WebAuthNGetCancellationId}), or null when
     * Windows gave none: that ceremony then cannot be canceled from here.
     */
    Cancellation newCancellation();

    /** {@code WebAuthNAuthenticatorMakeCredential}, owned by {@code request.pid}'s window. */
    Attestation makeCredential(MakeCredential request, Cancellation cancellation);

    /** {@code WebAuthNAuthenticatorGetAssertion}, owned by {@code request.pid}'s window. */
    Assertion getAssertion(GetAssertion request, Cancellation cancellation);

    /** One ceremony's cancellation id. */
    interface Cancellation {
        /** {@code WebAuthNCancelCurrentOperation}: thread-safe, and harmless when repeated. */
        void cancel();
    }

    /** A credential descriptor: its id and WEBAUTHN_CTAP_TRANSPORT_* bits (0: any). */
    final class Descriptor {
        public final byte[] id;
        public final int transports;

        public Descriptor(byte[] id, int transports) {
            this.id = id;
            this.transports = transports;
        }
    }

    /** The inputs of {@code WebAuthNAuthenticatorMakeCredential}. */
    final class MakeCredential {
        /** The requesting process: Windows' dialog is owned by its foreground window. */
        public int pid;
        public String rpId;
        public String rpName;
        public byte[] userId;
        public String userName;
        public String userDisplayName;
        /** COSE algorithm identifiers, in the relying party's order of preference. */
        public int[] algorithms;
        /** UTF-8 clientDataJSON; Windows hashes it with SHA-256. */
        public byte[] clientDataJson;
        public int timeoutMs;
        public Descriptor[] exclude = new Descriptor[0];
        public int attachment = ATTACHMENT_ANY;
        public int residentKey = RESIDENT_KEY_DISCOURAGED;
        public int userVerification = UV_PREFERRED;
        public int attestation = ATTESTATION_NONE;
    }

    /** The inputs of {@code WebAuthNAuthenticatorGetAssertion}. */
    final class GetAssertion {
        public int pid;
        public String rpId;
        public byte[] clientDataJson;
        public int timeoutMs;
        public Descriptor[] allow = new Descriptor[0];
        public int userVerification = UV_PREFERRED;
    }

    /** What every ceremony reports. */
    abstract class Outcome {
        /** Windows' HRESULT; anything but {@link #S_OK} means there is no result. */
        public int hresult = 0x80004005;  // E_FAIL until Windows answers
        /** {@code WebAuthNGetErrorName(hresult)}, or null when it was not asked. */
        public String errorName;
        /**
         * No call was made because the requesting process does not own the foreground
         * window, so Windows' dialog would have had no owner.
         */
        public boolean notForeground;
    }

    /** WEBAUTHN_CREDENTIAL_ATTESTATION, the parts a registration response is made of. */
    final class Attestation extends Outcome {
        public byte[] credentialId;
        public byte[] authenticatorData;
        public byte[] attestationObject;
        /** The transport that was used (one bit), or 0 when Windows did not say. */
        public int usedTransport;
        /** The authenticator's transports, or -1 when this Windows does not report them. */
        public int transports = -1;
        /** bResidentKey: 1 discoverable, 0 not, -1 when this Windows does not report it. */
        public int residentKey = -1;
    }

    /** WEBAUTHN_ASSERTION, the parts an authentication response is made of. */
    final class Assertion extends Outcome {
        public byte[] credentialId;
        public byte[] authenticatorData;
        public byte[] signature;
        /** The user handle; null or empty when the authenticator returned none. */
        public byte[] userId;
        /** The transport that was used (one bit), or 0 when Windows did not say. */
        public int usedTransport;
    }
}
