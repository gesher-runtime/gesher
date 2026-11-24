/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.ComponentName;
import android.credentials.ClearCredentialStateRequest;
import android.credentials.CreateCredentialException;
import android.credentials.CreateCredentialRequest;
import android.credentials.CreateCredentialResponse;
import android.credentials.Credential;
import android.credentials.CredentialOption;
import android.credentials.CredentialProviderInfo;
import android.credentials.GetCredentialException;
import android.credentials.GetCredentialRequest;
import android.credentials.GetCredentialResponse;
import android.credentials.IClearCredentialStateCallback;
import android.credentials.ICreateCredentialCallback;
import android.credentials.ICredentialManager;
import android.credentials.IGetCandidateCredentialsCallback;
import android.credentials.IGetCredentialCallback;
import android.credentials.IPrepareGetCredentialCallback;
import android.credentials.ISetEnabledProvidersCallback;
import android.credentials.PrepareGetCredentialResponseInternal;
import android.credentials.RegisterCredentialDescriptionRequest;
import android.credentials.UnregisterCredentialDescriptionRequest;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.IBinder;
import android.os.ICancellationSignal;
import android.os.RemoteException;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Android's Credential Manager ({@code credential}, {@code ICredentialManager}) for passkeys,
 * over Windows' WebAuthn API (docs/passkeys.md, ADR 0092).
 *
 * <p>AOSP's CredentialManagerService hands a request to the device's credential providers
 * and shows their entries in a selector. AOSP has no passkey provider of its own. Here the
 * system server is the one provider: a public-key request becomes one Windows WebAuthn
 * ceremony ({@link PasskeyBackend}), whose own dialog is the selector (Windows Hello, a
 * security key, a phone). What applications observe is the Binder contract of
 * android-16.0.0_r4's service and the response and error types androidx.credentials reads.
 *
 * <p>Truthfulness and security:
 * <ul>
 * <li>A response is made only of bytes Windows returned. Cancellation, a missing credential,
 *     a missing authenticator and every other failure are reported as what they are.
 * <li>The application never chooses its WebAuthn origin: clientDataJSON carries
 *     {@code android:apk-key-hash:} of its verified signing certificate.
 * <li>Before any ceremony the relying party must grant the application
 *     {@code delegate_permission/common.get_login_creds} ({@link AssetLinks}).
 * <li>Without Windows' WebAuthn API, and for every credential type but public keys, the
 *     answers are AOSP's for a device without a provider.
 * </ul>
 */
public final class CredentialManagerService extends ICredentialManager.Default {
    /** androidx.credentials.PublicKeyCredential.TYPE_PUBLIC_KEY_CREDENTIAL. */
    static final String TYPE_PUBLIC_KEY_CREDENTIAL = "androidx.credentials.TYPE_PUBLIC_KEY_CREDENTIAL";
    /** The request and response Bundle keys androidx.credentials uses for that type. */
    static final String KEY_REQUEST_JSON = "androidx.credentials.BUNDLE_KEY_REQUEST_JSON";
    static final String KEY_REGISTRATION_RESPONSE_JSON =
            "androidx.credentials.BUNDLE_KEY_REGISTRATION_RESPONSE_JSON";
    static final String KEY_AUTHENTICATION_RESPONSE_JSON =
            "androidx.credentials.BUNDLE_KEY_AUTHENTICATION_RESPONSE_JSON";
    /** The error types androidx.credentials parses back into DOM exceptions. */
    static final String CREATE_DOM_EXCEPTION =
            "androidx.credentials.TYPE_CREATE_PUBLIC_KEY_CREDENTIAL_DOM_EXCEPTION";
    static final String GET_DOM_EXCEPTION =
            "androidx.credentials.TYPE_GET_PUBLIC_KEY_CREDENTIAL_DOM_EXCEPTION";

    static final String PERMISSION_SET_ORIGIN = "android.permission.CREDENTIAL_MANAGER_SET_ORIGIN";
    static final String PERMISSION_SET_ALLOWED_PROVIDERS =
            "android.permission.CREDENTIAL_MANAGER_SET_ALLOWED_PROVIDERS";
    static final String PERMISSION_QUERY_CANDIDATES =
            "android.permission.CREDENTIAL_MANAGER_QUERY_CANDIDATE_CREDENTIALS";

    // AOSP's messages for a device where no provider serves the request.
    private static final String NO_CREDENTIAL = "No credentials available on this device.";
    private static final String NO_CREATE_OPTIONS = "No create options available.";
    /** What Android's providers answer when the caller is not associated with the site. */
    private static final String NOT_VALIDATED = "The incoming request cannot be validated";

    // winerror.h
    private static final int NTE_EXISTS = 0x8009000F;
    private static final int NTE_NOT_FOUND = 0x80090011;
    private static final int NTE_DEVICE_NOT_FOUND = 0x80090030;
    private static final int NTE_USER_CANCELLED = 0x80090036;
    private static final int E_CANCELLED = 0x800704C7;  // HRESULT_FROM_WIN32(ERROR_CANCELLED)

    /** What the service needs from the system server. */
    public interface Host {
        int callingUid();

        int callingPid();

        /** The uid {@code packageName} is installed with, or -1 when it is not installed. */
        int packageUid(String packageName);

        boolean hasPermission(String permission, int pid, int uid);

        /**
         * The DER certificates {@code packageName} is currently signed with, from a verified
         * APK signature; null when there is none.
         */
        byte[][] signingCertificates(String packageName);

        /** Runs a user-paced ceremony off the Binder thread. Ceremonies run concurrently. */
        void execute(Runnable ceremony);
    }

    private final Host mHost;
    private final PasskeyBackend mBackend;
    private final AssetLinks.Fetcher mFetcher;

    /** {@code backend} is null when this runtime has no WebAuthn natives. */
    public CredentialManagerService(Host host, PasskeyBackend backend, AssetLinks.Fetcher fetcher) {
        mHost = host;
        mBackend = backend;
        mFetcher = fetcher;
    }

    /** Whether Windows has a WebAuthn API to serve public-key requests with. */
    private boolean passkeysAvailable() {
        return mBackend != null && mBackend.apiVersion() > 0;
    }

    @Override
    public ICancellationSignal executeGetCredential(GetCredentialRequest request,
            IGetCredentialCallback callback, String callingPackage) {
        ICancellationSignal transport = CancellationSignal.createTransport();
        int uid = mHost.callingUid();
        int pid = mHost.callingPid();
        enforceCallingPackage(callingPackage, uid);
        validateGetCredentialRequest(request, pid, uid);

        String requestJson = null;
        // A caller that sets an origin acts for a web site. Android's providers accept that
        // only from the browsers on their allowlists; Gesher has no such list, so this
        // provider declines, which AOSP reports as no provider being able to answer.
        if (request.getOrigin() == null && passkeysAvailable()) {
            for (CredentialOption option : request.getCredentialOptions()) {
                if (!TYPE_PUBLIC_KEY_CREDENTIAL.equals(option.getType())) continue;
                requestJson = option.getCredentialRetrievalData().getString(KEY_REQUEST_JSON);
                if (requestJson != null) break;
            }
        }
        if (requestJson == null) {
            try {
                callback.onError(GetCredentialException.TYPE_NO_CREDENTIAL, NO_CREDENTIAL);
            } catch (RemoteException gone) {
                // The caller died; nobody is waiting.
            }
            return transport;
        }
        String json = requestJson;
        Session session = new Session(transport, callback.asBinder()) {
            @Override
            void error(String type, String message) throws RemoteException {
                callback.onError(type, message);
            }
        };
        mHost.execute(() -> {
            try {
                Bundle data = authenticate(session, json, callingPackage, pid);
                if (data == null) return;  // canceled by the application
                GetCredentialResponse response =
                        new GetCredentialResponse(new Credential(TYPE_PUBLIC_KEY_CREDENTIAL, data));
                session.respond(() -> callback.onResponse(response));
            } catch (Failure failure) {
                session.fail(failure.type(false), failure.getMessage());
            } catch (RuntimeException unexpected) {
                session.fail(GetCredentialException.TYPE_UNKNOWN, unexpected.toString());
            }
        });
        return transport;
    }

    @Override
    public ICancellationSignal executeCreateCredential(CreateCredentialRequest request,
            ICreateCredentialCallback callback, String callingPackage) {
        ICancellationSignal transport = CancellationSignal.createTransport();
        int uid = mHost.callingUid();
        int pid = mHost.callingPid();
        if (request.getOrigin() != null) enforcePermission(PERMISSION_SET_ORIGIN, pid, uid);
        enforceCallingPackage(callingPackage, uid);

        String requestJson = null;
        // See executeGetCredential for a caller-set origin.
        if (request.getOrigin() == null && passkeysAvailable()
                && TYPE_PUBLIC_KEY_CREDENTIAL.equals(request.getType())) {
            requestJson = request.getCredentialData().getString(KEY_REQUEST_JSON);
        }
        if (requestJson == null) {
            try {
                callback.onError(CreateCredentialException.TYPE_NO_CREATE_OPTIONS,
                        NO_CREATE_OPTIONS);
            } catch (RemoteException gone) {
                // The caller died; nobody is waiting.
            }
            return transport;
        }
        String json = requestJson;
        Session session = new Session(transport, callback.asBinder()) {
            @Override
            void error(String type, String message) throws RemoteException {
                callback.onError(type, message);
            }
        };
        mHost.execute(() -> {
            try {
                Bundle data = register(session, json, callingPackage, pid);
                if (data == null) return;  // canceled by the application
                CreateCredentialResponse response = new CreateCredentialResponse(data);
                session.respond(() -> callback.onResponse(response));
            } catch (Failure failure) {
                session.fail(failure.type(true), failure.getMessage());
            } catch (RuntimeException unexpected) {
                session.fail(CreateCredentialException.TYPE_UNKNOWN, unexpected.toString());
            }
        });
        return transport;
    }

    /**
     * The prepare/show pair needs a selector the application launches later through a
     * PendingIntent. Gesher has none: the answer is AOSP's for a request no provider can
     * serve (no results, no PendingIntent), and the application's later getCredential on
     * that handle reports TYPE_NO_CREDENTIAL itself.
     */
    @Override
    public ICancellationSignal executePrepareGetCredential(GetCredentialRequest request,
            IPrepareGetCredentialCallback prepareGetCredentialCallback,
            IGetCredentialCallback getCredentialCallback, String callingPackage) {
        ICancellationSignal transport = CancellationSignal.createTransport();
        int uid = mHost.callingUid();
        int pid = mHost.callingPid();
        validateGetCredentialRequest(request, pid, uid);
        enforceCallingPackage(callingPackage, uid);
        try {
            prepareGetCredentialCallback.onResponse(new PrepareGetCredentialResponseInternal(
                    mHost.hasPermission(PERMISSION_QUERY_CANDIDATES, pid, uid),
                    /* credentialResultTypes= */ null, /* hasAuthenticationResults= */ false,
                    /* hasRemoteResults= */ false, /* pendingIntent= */ null));
        } catch (RemoteException gone) {
            // The caller died; nobody is waiting.
        }
        return transport;
    }

    /** Only the device's credential autofill service may ask; Gesher has none. */
    @Override
    public ICancellationSignal getCandidateCredentials(GetCredentialRequest request,
            IGetCandidateCredentialsCallback callback, IBinder clientCallback,
            String callingPackage) {
        throw new SecurityException("Credential Autofill service does not exist on this device.");
    }

    /**
     * Asks the providers to forget what they keep about the application's sign-in (an
     * automatic choice, a session). This provider keeps nothing between ceremonies, so with
     * it there is nothing left to clear; without it no provider takes the request, which
     * AOSP reports with this error.
     */
    @Override
    public ICancellationSignal clearCredentialState(ClearCredentialStateRequest request,
            IClearCredentialStateCallback callback, String callingPackage) {
        enforceCallingPackage(callingPackage, mHost.callingUid());
        ICancellationSignal transport = CancellationSignal.createTransport();
        try {
            if (passkeysAvailable()) {
                callback.onSuccess();
            } else {
                callback.onError("UNKNOWN", "No credentials available on this device");
            }
        } catch (RemoteException gone) {
            // The caller died; nobody is waiting.
        }
        return transport;
    }

    /**
     * Third-party provider services are not hosted, so the enabled-provider settings are
     * not stored: a caller that may write them is told the store failed, in AOSP's words.
     */
    @Override
    public void setEnabledProviders(List<String> primaryProviders, List<String> providers,
            int userId, ISetEnabledProvidersCallback callback) {
        try {
            if (!mHost.hasPermission("android.permission.WRITE_SECURE_SETTINGS",
                    mHost.callingPid(), mHost.callingUid())) {
                callback.onError("permission_denied",
                        "Caller is missing WRITE_SECURE_SETTINGS permission");
            } else {
                callback.onError("failed_setting_store",
                        "Failed to store setting containing enabled or primary providers");
            }
        } catch (RemoteException gone) {
            // The caller died; nobody is waiting.
        }
    }

    @Override
    public void registerCredentialDescription(RegisterCredentialDescriptionRequest request,
            String callingPackage) {
        // DeviceConfig's enable_credential_description_api, false by default.
        throw new UnsupportedOperationException("Feature not supported");
    }

    @Override
    public void unregisterCredentialDescription(UnregisterCredentialDescriptionRequest request,
            String callingPackage) {
        throw new UnsupportedOperationException("Feature not supported");
    }

    @Override
    public boolean isEnabledCredentialProviderService(ComponentName componentName,
            String callingPackage) {
        enforceCallingPackage(callingPackage, mHost.callingUid());
        if (componentName == null) return false;
        if (!componentName.getPackageName().equals(callingPackage)) {
            throw new IllegalArgumentException("provided component name does not match"
                    + " does not match requesting component");
        }
        return false;  // no provider service is enabled: Settings.Secure.CREDENTIAL_SERVICE is unset
    }

    @Override
    public List<CredentialProviderInfo> getCredentialProviderServices(int userId,
            int providerFilter) {
        verifyGetProvidersPermission();
        return new ArrayList<>();
    }

    @Override
    public List<CredentialProviderInfo> getCredentialProviderServicesForTesting(
            int providerFilter) {
        verifyGetProvidersPermission();
        return new ArrayList<>();
    }

    /** DeviceConfig's enable_credential_manager, true by default. */
    @Override
    public boolean isServiceEnabled() {
        return true;
    }

    private void verifyGetProvidersPermission() {
        int pid = mHost.callingPid();
        int uid = mHost.callingUid();
        if (mHost.hasPermission("android.permission.QUERY_ALL_PACKAGES", pid, uid)
                || mHost.hasPermission("android.permission.LIST_ENABLED_CREDENTIAL_PROVIDERS",
                        pid, uid)) {
            return;
        }
        throw new SecurityException("Caller is missing permission: QUERY_ALL_PACKAGES or "
                + "LIST_ENABLED_CREDENTIAL_PROVIDERS");
    }

    private void validateGetCredentialRequest(GetCredentialRequest request, int pid, int uid) {
        if (request.getOrigin() != null) enforcePermission(PERMISSION_SET_ORIGIN, pid, uid);
        for (CredentialOption option : request.getCredentialOptions()) {
            if (option.getAllowedProviders() != null && !option.getAllowedProviders().isEmpty()) {
                enforcePermission(PERMISSION_SET_ALLOWED_PROVIDERS, pid, uid);
                break;
            }
        }
    }

    private void enforcePermission(String permission, int pid, int uid) {
        if (!mHost.hasPermission(permission, pid, uid)) {
            // Context.enforceCallingPermission's message.
            throw new SecurityException("Permission Denial: Neither user " + uid
                    + " nor current process has " + permission + ".");
        }
    }

    private void enforceCallingPackage(String callingPackage, int callingUid) {
        int packageUid = callingPackage == null ? -1 : mHost.packageUid(callingPackage);
        if (packageUid < 0) throw new SecurityException(callingPackage + " not found");
        if (packageUid != callingUid) {
            throw new SecurityException(callingPackage + " does not belong to uid " + callingUid);
        }
    }

    /** A ceremony's failure, as the error type and message the application receives. */
    private static final class Failure extends Exception {
        private static final long serialVersionUID = 1;
        /** A complete error type, or null when {@code mDomError} names a DOMException. */
        private final String mCreateType;
        private final String mGetType;
        private final String mDomError;

        private Failure(String createType, String getType, String domError, String message) {
            super(message);
            mCreateType = createType;
            mGetType = getType;
            mDomError = domError;
        }

        static Failure dom(String name, String message) {
            return new Failure(null, null, name, message);
        }

        static Failure framework(String createType, String getType, String message) {
            return new Failure(createType, getType, null, message);
        }

        String type(boolean create) {
            if (mDomError == null) return create ? mCreateType : mGetType;
            return (create ? CREATE_DOM_EXCEPTION : GET_DOM_EXCEPTION) + "/" + domType(mDomError);
        }
    }

    /**
     * androidx.credentials' type of a DOMException name: NotAllowedError is
     * androidx.credentials.TYPE_NOT_ALLOWED_ERROR.
     */
    static String domType(String name) {
        StringBuilder type = new StringBuilder("androidx.credentials.TYPE");
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            if (Character.isUpperCase(c)) type.append('_');
            type.append(Character.toUpperCase(c));
        }
        return type.toString();
    }

    /**
     * Why Windows produced no result. User cancellation, a missing credential and a missing
     * authenticator have Android framework types; the rest keep the DOMException name
     * Windows gives the HRESULT ({@code WebAuthNGetErrorName}).
     */
    private static Failure failure(boolean create, PasskeyBackend.Outcome outcome) {
        if (outcome.notForeground) {
            // Android's selector cannot be started for such a caller either
            // (CredentialManager's startIntentSender failure).
            return Failure.framework(CreateCredentialException.TYPE_UNKNOWN,
                    GetCredentialException.TYPE_UNKNOWN,
                    "The calling application does not own the foreground window");
        }
        int hr = outcome.hresult;
        String detail = "Windows WebAuthn: "
                + (outcome.errorName == null ? "error" : outcome.errorName)
                + String.format(Locale.ROOT, " (0x%08X)", hr);
        if (hr == NTE_USER_CANCELLED || hr == E_CANCELLED) {
            return Failure.framework(CreateCredentialException.TYPE_USER_CANCELED,
                    GetCredentialException.TYPE_USER_CANCELED, detail);
        }
        if (!create && (hr == NTE_NOT_FOUND || hr == NTE_DEVICE_NOT_FOUND)) {
            return Failure.framework(null, GetCredentialException.TYPE_NO_CREDENTIAL, detail);
        }
        if (create && hr == NTE_DEVICE_NOT_FOUND) {
            return Failure.framework(CreateCredentialException.TYPE_NO_CREATE_OPTIONS, null,
                    detail);
        }
        if (create && hr == NTE_EXISTS) {
            return Failure.dom("InvalidStateError", detail);  // an excluded credential exists
        }
        if (outcome.errorName != null) {
            switch (outcome.errorName) {
                case "InvalidStateError":
                case "ConstraintError":
                case "NotSupportedError":
                case "NotAllowedError":
                case "UnknownError":
                    return Failure.dom(outcome.errorName, detail);
                default:
                    break;
            }
        }
        return Failure.framework(CreateCredentialException.TYPE_UNKNOWN,
                GetCredentialException.TYPE_UNKNOWN, detail);
    }

    /** The caller's WebAuthn origin, once the relying party is known to have delegated to it. */
    private String associatedOrigin(String rpId, String callingPackage) throws Failure {
        byte[][] certificates = mHost.signingCertificates(callingPackage);
        if (certificates == null || certificates.length == 0
                || !AssetLinks.verify(mFetcher, rpId, callingPackage, certificates)) {
            throw Failure.dom(PasskeyRequests.SECURITY_ERROR, NOT_VALIDATED);
        }
        return PasskeyRequests.origin(certificates[0]);
    }

    /** One registration ceremony; null when the application canceled it. */
    private Bundle register(Session session, String json, String callingPackage, int pid)
            throws Failure {
        PasskeyRequests.Creation creation;
        try {
            creation = PasskeyRequests.parseCreation(json);
        } catch (PasskeyRequests.Refusal refusal) {
            throw Failure.dom(refusal.domError, refusal.getMessage());
        }
        PasskeyBackend.MakeCredential request = creation.request;
        String origin = associatedOrigin(request.rpId, callingPackage);
        request.pid = pid;
        request.clientDataJson =
                PasskeyRequests.clientData("webauthn.create", creation.challenge, origin);
        PasskeyBackend.Cancellation cancellation = mBackend.newCancellation();
        if (!session.begin(cancellation)) return null;
        PasskeyBackend.Attestation attestation = mBackend.makeCredential(request, cancellation);
        if (attestation == null) attestation = new PasskeyBackend.Attestation();
        if (attestation.hresult != PasskeyBackend.S_OK) throw failure(true, attestation);
        Bundle data = new Bundle();
        try {
            data.putString(KEY_REGISTRATION_RESPONSE_JSON, PasskeyRequests.registrationResponse(
                    request.clientDataJson, attestation, creation.credProps));
        } catch (RuntimeException unreadable) {
            throw Failure.framework(CreateCredentialException.TYPE_UNKNOWN, null,
                    "Windows returned an attestation that cannot be read: "
                            + unreadable.getMessage());
        }
        return data;
    }

    /** One assertion ceremony; null when the application canceled it. */
    private Bundle authenticate(Session session, String json, String callingPackage, int pid)
            throws Failure {
        PasskeyRequests.Authentication authentication;
        try {
            authentication = PasskeyRequests.parseAuthentication(json);
        } catch (PasskeyRequests.Refusal refusal) {
            throw Failure.dom(refusal.domError, refusal.getMessage());
        }
        PasskeyBackend.GetAssertion request = authentication.request;
        String origin = associatedOrigin(request.rpId, callingPackage);
        request.pid = pid;
        request.clientDataJson =
                PasskeyRequests.clientData("webauthn.get", authentication.challenge, origin);
        PasskeyBackend.Cancellation cancellation = mBackend.newCancellation();
        if (!session.begin(cancellation)) return null;
        PasskeyBackend.Assertion assertion = mBackend.getAssertion(request, cancellation);
        if (assertion == null) assertion = new PasskeyBackend.Assertion();
        if (assertion.hresult != PasskeyBackend.S_OK) throw failure(false, assertion);
        if (assertion.credentialId == null || assertion.authenticatorData == null
                || assertion.signature == null) {
            throw Failure.framework(null, GetCredentialException.TYPE_UNKNOWN,
                    "Windows returned an incomplete assertion");
        }
        Bundle data = new Bundle();
        data.putString(KEY_AUTHENTICATION_RESPONSE_JSON,
                PasskeyRequests.authenticationResponse(request.clientDataJson, assertion));
        return data;
    }

    /**
     * One request, as AOSP's RequestSession: it answers its callback at most once, and not
     * at all once the application canceled it or died. Either of those also cancels
     * Windows' dialog.
     */
    private abstract static class Session implements IBinder.DeathRecipient {
        /** A callback invocation. */
        interface Reply {
            void send() throws RemoteException;
        }

        private final IBinder mClient;
        private boolean mCanceled;
        private boolean mComplete;
        private PasskeyBackend.Cancellation mCancellation;

        Session(ICancellationSignal transport, IBinder client) {
            mClient = client;
            CancellationSignal.fromTransport(transport).setOnCancelListener(this::cancel);
            try {
                if (client != null) client.linkToDeath(this, 0);
            } catch (RemoteException alreadyDead) {
                cancel();
            }
        }

        abstract void error(String type, String message) throws RemoteException;

        @Override
        public void binderDied() {
            cancel();
        }

        private void cancel() {
            PasskeyBackend.Cancellation cancellation;
            synchronized (this) {
                if (mCanceled || mComplete) return;
                mCanceled = true;
                cancellation = mCancellation;
            }
            if (cancellation != null) cancellation.cancel();
        }

        /** Whether the ceremony may start; false once the application canceled. */
        synchronized boolean begin(PasskeyBackend.Cancellation cancellation) {
            if (mCanceled) return false;
            mCancellation = cancellation;
            return true;
        }

        void respond(Reply reply) {
            synchronized (this) {
                if (mCanceled || mComplete) return;
                mComplete = true;
                mCancellation = null;
            }
            if (mClient != null) mClient.unlinkToDeath(this, 0);
            try {
                reply.send();
            } catch (RemoteException gone) {
                // The caller died; nobody is waiting.
            }
        }

        void fail(String type, String message) {
            respond(() -> error(type, message));
        }
    }
}
