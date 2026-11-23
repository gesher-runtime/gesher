/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.Manifest;
import android.hardware.biometrics.BiometricAuthenticator;
import android.hardware.biometrics.BiometricConstants;
import android.hardware.biometrics.BiometricEnrollmentStatusInternal;
import android.hardware.biometrics.BiometricManager;
import android.hardware.biometrics.BiometricManager.Authenticators;
import android.hardware.biometrics.BiometricPrompt;
import android.hardware.biometrics.IAuthService;
import android.hardware.biometrics.IBiometricServiceReceiver;
import android.hardware.biometrics.PromptInfo;
import android.os.Binder;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.UserHandle;

import java.util.ArrayList;
import java.util.List;

/**
 * {@code auth}: IAuthService, what BiometricManager and BiometricPrompt talk to (ADR 0091).
 *
 * <p>AOSP's SystemServer starts AuthService and BiometricService on every device, sensors
 * or not, so this service is always registered. Gesher registers no biometric sensor:
 * Windows Hello's verifier does not say whether a biometric or the PIN was used and binds
 * no key, so neither BIOMETRIC_WEAK nor BIOMETRIC_STRONG can be claimed. The answers are
 * those of AOSP's PreAuthInfo for a device without sensors: biometrics are
 * {@code BIOMETRIC_ERROR_NO_HARDWARE}; the device credential is available exactly when
 * {@code trust} says the device is secure ({@link Host#isDeviceSecure}, as PreAuthInfo's
 * {@code credentialAvailable}), and is then confirmed through {@link WindowsHelloBackend}.
 * With no backend (the natives are not there) a confirmation ends with
 * {@code BIOMETRIC_ERROR_HW_UNAVAILABLE}; nothing succeeds without a real verification.
 *
 * <p>Like BiometricService, one session at a time: a new prompt that can start cancels
 * the current one (BIOMETRIC_ERROR_CANCELED to its client), {@link #cancelAuthentication}
 * ends the current one with BIOMETRIC_ERROR_CANCELED, the client's death ends it without a
 * callback, and each request gets exactly one terminal callback. Work after the binder
 * call runs on {@link Host#post}, as BiometricService's handler.
 */
public final class AuthService extends IAuthService.Default {
    /** What the service asks of the rest of the system server. */
    public interface Host {
        /** Whether pid/uid holds {@code permission} (the system server holds all). */
        boolean hasPermission(String permission, int pid, int uid);

        /** AppOpsManager.OP_USE_BIOMETRIC noted for uid and package: allowed or not. */
        boolean noteUseBiometric(int uid, String packageName);

        /** Whether the caller owns the foreground window (BiometricService's isForeground). */
        boolean isForeground(int uid, int pid);

        /** ITrustManager.isDeviceSecure: the device has a credential the user can confirm. */
        boolean isDeviceSecure(int userId);

        /** A framework string, by its com.android.internal.R.string name. */
        CharSequence string(String name);

        /** Runs {@code work} later, in order, on the service's thread. */
        void post(Runnable work);
    }

    static final String USE_BIOMETRIC_INTERNAL = "android.permission.USE_BIOMETRIC_INTERNAL";
    static final String TEST_BIOMETRIC = "android.permission.TEST_BIOMETRIC";
    static final String SET_BIOMETRIC_DIALOG_ADVANCED =
            "android.permission.SET_BIOMETRIC_DIALOG_ADVANCED";

    // PreAuthInfo's AuthenticatorStatus values that a device without sensors can produce.
    private static final int AUTHENTICATOR_OK = 1;
    private static final int BIOMETRIC_NO_HARDWARE = 2;
    private static final int CREDENTIAL_NOT_ENROLLED = 9;
    private static final int MANDATORY_BIOMETRIC_UNAVAILABLE_ERROR = 13;

    private final Host mHost;
    private final WindowsHelloBackend mBackend;  // null: no Windows Hello natives
    private final Object mLock = new Object();
    private long mNextRequestId = 1;  // guarded by mLock
    private Session mSession;         // guarded by mLock: the prompt being shown

    public AuthService(Host host, WindowsHelloBackend backend) {
        mHost = java.util.Objects.requireNonNull(host);
        mBackend = backend;
    }

    // --- binder calls ---------------------------------------------------------------------

    @Override
    public long authenticate(IBinder token, long operationId, int userId,
            IBiometricServiceReceiver receiver, String opPackageName, PromptInfo promptInfo) {
        final int uid = Binder.getCallingUid();
        final int pid = Binder.getCallingPid();
        checkUser(userId);
        // AuthService.authenticate: these end the request with CANCELED, not an exception.
        if (!mHost.noteUseBiometric(uid, opPackageName)) return fastFail(receiver);
        if (token == null || receiver == null || opPackageName == null || promptInfo == null) {
            return fastFail(receiver);
        }
        if (!mHost.isForeground(uid, pid)) return fastFail(receiver);
        if (promptInfo.requiresTestOrInternalPermission()
                && !mHost.hasPermission(TEST_BIOMETRIC, pid, uid)) {
            enforce(USE_BIOMETRIC_INTERNAL);
        }
        if (promptInfo.requiresInternalPermission()) enforce(USE_BIOMETRIC_INTERNAL);
        if (promptInfo.requiresAdvancedPermission()) enforce(SET_BIOMETRIC_DIALOG_ADVANCED);
        // BiometricService.authenticate.
        validate(promptInfo.getAuthenticators());
        // Utils.combineAuthenticatorBundles, on a copy of the value: the PromptInfo is the
        // app's own object when the app runs in the system server's process.
        int authenticators = promptInfo.getAuthenticators();
        if (authenticators == 0) {
            authenticators = promptInfo.isDeviceCredentialAllowed()
                    ? Authenticators.DEVICE_CREDENTIAL | Authenticators.BIOMETRIC_WEAK
                    : Authenticators.BIOMETRIC_WEAK;
        }
        final long requestId;
        synchronized (mLock) {
            requestId = mNextRequestId++;
        }
        final Session session = new Session(requestId, token, receiver, operationId, userId,
                authenticators, new WindowsHelloBackend.Request(uid, pid, opPackageName,
                        message(promptInfo)));
        mHost.post(() -> start(session));
        return requestId;
    }

    @Override
    public void cancelAuthentication(IBinder token, String opPackageName, long requestId) {
        checkPermission();
        if (token == null || opPackageName == null) return;
        mHost.post(() -> cancel(token, requestId));
    }

    @Override
    public int canAuthenticate(String opPackageName, int userId, int authenticators) {
        checkUser(userId);
        validate(authenticators);
        return toBiometricManager(toBiometricConstant(status(authenticators, userId)[1]));
    }

    @Override
    public long getLastAuthenticationTime(int userId, int authenticators) {
        checkUser(userId);
        // No gatekeeper secure user id and no keystore auth tokens (ADR 0091 phase 3):
        // BiometricService's answer for a user without one.
        return BiometricManager.BIOMETRIC_NO_AUTHENTICATION;
    }

    @Override
    public boolean hasEnrolledBiometrics(int userId, String opPackageName) {
        enforce(USE_BIOMETRIC_INTERNAL);
        return false;
    }

    @Override
    public List<BiometricEnrollmentStatusInternal> getEnrollmentStatusList(int userId,
            String opPackageName) {
        enforce(SET_BIOMETRIC_DIALOG_ADVANCED);
        if (userId != UserHandle.getCallingUserId()) enforce(USE_BIOMETRIC_INTERNAL);
        return new ArrayList<>();  // no fingerprint or face service
    }

    @Override
    public long[] getAuthenticatorIds(int userId) {
        // Unchecked for the caller's own user, as AOSP's (the in-app keystore calls it).
        if (userId != UserHandle.getCallingUserId()) enforce(USE_BIOMETRIC_INTERNAL);
        return new long[0];  // no sensors
    }

    @Override
    public CharSequence getButtonLabel(int userId, String opPackageName, int authenticators) {
        return credentialString(userId, authenticators, "screen_lock_app_setting_name");
    }

    @Override
    public CharSequence getPromptMessage(int userId, String opPackageName, int authenticators) {
        return credentialString(userId, authenticators, "screen_lock_dialog_default_subtitle");
    }

    @Override
    public CharSequence getSettingName(int userId, String opPackageName, int authenticators) {
        return credentialString(userId, authenticators, "screen_lock_app_setting_name");
    }

    // --- sessions, on the service's thread ------------------------------------------------

    /** One BiometricPrompt request: its client, and the verification once it runs. */
    private final class Session implements IBinder.DeathRecipient {
        final long id;
        final IBinder token;
        final IBiometricServiceReceiver receiver;
        final long operationId;
        final int userId;
        final int authenticators;
        final WindowsHelloBackend.Request request;
        WindowsHelloBackend.Verification verification;  // guarded by mLock
        boolean linked;

        Session(long id, IBinder token, IBiometricServiceReceiver receiver, long operationId,
                int userId, int authenticators, WindowsHelloBackend.Request request) {
            this.id = id;
            this.token = token;
            this.receiver = receiver;
            this.operationId = operationId;
            this.userId = userId;
            this.authenticators = authenticators;
            this.request = request;
        }

        @Override
        public void binderDied() {
            mHost.post(() -> died(this));
        }

        void unlink() {
            if (!linked) return;
            linked = false;
            receiver.asBinder().unlinkToDeath(this, 0);
        }
    }

    /** BiometricService.handleAuthenticate. */
    private void start(Session session) {
        int[] status = status(session.authenticators, session.userId);
        if (status[1] != AUTHENTICATOR_OK) {
            // getPreAuthenticateStatus: the current prompt, if any, keeps running.
            error(session, status[0], toBiometricConstant(status[1]));
            return;
        }
        // Only a device credential can be eligible here (there are no sensors).
        Session previous;
        synchronized (mLock) {
            previous = mSession;
            mSession = null;
        }
        if (previous != null) end(previous, BiometricConstants.BIOMETRIC_ERROR_CANCELED);
        int unavailable = unavailable(session);
        if (unavailable != BiometricConstants.BIOMETRIC_SUCCESS) {
            error(session, BiometricAuthenticator.TYPE_CREDENTIAL, unavailable);
            return;
        }
        try {
            session.receiver.asBinder().linkToDeath(session, 0);
            session.linked = true;
        } catch (RemoteException dead) {
            return;  // the client is gone: nobody to answer
        }
        synchronized (mLock) {
            mSession = session;
            try {
                session.verification = mBackend.verify(session.request,
                        result -> mHost.post(() -> verified(session, result)));
            } catch (RuntimeException e) {
                SystemServer.log("auth: Windows Hello verification did not start: " + e);
                mHost.post(() -> verified(session, WindowsHelloBackend.FAILED));
            }
        }
    }

    /**
     * Why a credential confirmation cannot be offered, or BIOMETRIC_SUCCESS. A request bound
     * to a keystore operation cannot be honoured: Gesher's keystore cannot receive the auth
     * token AuthSession would add (ADR 0091), so it is never reported as confirmed.
     */
    private int unavailable(Session session) {
        if (session.operationId != 0 || mBackend == null) {
            return BiometricConstants.BIOMETRIC_ERROR_HW_UNAVAILABLE;
        }
        switch (mBackend.availability()) {
            case WindowsHelloBackend.AVAILABLE:
                return BiometricConstants.BIOMETRIC_SUCCESS;
            case WindowsHelloBackend.NOT_CONFIGURED_FOR_USER:
                return BiometricConstants.BIOMETRIC_ERROR_NO_DEVICE_CREDENTIAL;
            default:
                return BiometricConstants.BIOMETRIC_ERROR_HW_UNAVAILABLE;
        }
    }

    private void verified(Session session, int result) {
        synchronized (mLock) {
            if (mSession != session) return;  // canceled, replaced or its client died
            mSession = null;
        }
        session.unlink();
        if (result == WindowsHelloBackend.VERIFIED) {
            try {
                session.receiver.onAuthenticationSucceeded(
                        BiometricPrompt.AUTHENTICATION_RESULT_TYPE_DEVICE_CREDENTIAL);
            } catch (RemoteException e) {
                SystemServer.log("auth: client gone before its success: " + e);
            }
            return;
        }
        error(session, BiometricAuthenticator.TYPE_CREDENTIAL, toError(result));
    }

    /** BiometricService.handleCancelAuthentication: only the current request. */
    private void cancel(IBinder token, long requestId) {
        Session session;
        synchronized (mLock) {
            session = mSession;
            if (session == null || session.id != requestId || session.token != token) return;
            mSession = null;
        }
        end(session, BiometricConstants.BIOMETRIC_ERROR_CANCELED);
    }

    private void died(Session session) {
        WindowsHelloBackend.Verification verification;
        synchronized (mLock) {
            if (mSession != session) return;
            mSession = null;
            verification = session.verification;
        }
        session.linked = false;
        if (verification != null) verification.cancel();
    }

    /** Ends a session that was current: closes Windows' prompt, then tells the client. */
    private void end(Session session, int error) {
        WindowsHelloBackend.Verification verification;
        synchronized (mLock) {
            verification = session.verification;
        }
        if (verification != null) verification.cancel();
        session.unlink();
        error(session, BiometricAuthenticator.TYPE_CREDENTIAL, error);
    }

    private static void error(Session session, int modality, int error) {
        try {
            session.receiver.onError(modality, error, 0 /* vendorCode */);
        } catch (RemoteException e) {
            SystemServer.log("auth: client gone before its error " + error + ": " + e);
        }
    }

    // --- AOSP's rules ---------------------------------------------------------------------

    /** Windows' result → BiometricConstants (ADR 0091's table). */
    static int toError(int result) {
        switch (result) {
            case WindowsHelloBackend.CANCELED:
                // An app's own cancel was answered already; this is the user's.
                return BiometricConstants.BIOMETRIC_ERROR_USER_CANCELED;
            case WindowsHelloBackend.RETRIES_EXHAUSTED:
                return BiometricConstants.BIOMETRIC_ERROR_LOCKOUT;
            case WindowsHelloBackend.NOT_CONFIGURED_FOR_USER:
                return BiometricConstants.BIOMETRIC_ERROR_NO_DEVICE_CREDENTIAL;
            default:  // DeviceNotPresent, DisabledByPolicy, DeviceBusy, FAILED
                return BiometricConstants.BIOMETRIC_ERROR_HW_UNAVAILABLE;
        }
    }

    /**
     * PreAuthInfo.getInternalStatus with no biometric sensor: {modality, status}.
     * PreAuthInfo asks ITrustManager.isDeviceSecure for credentialAvailable.
     */
    private int[] status(int authenticators, int userId) {
        boolean biometric = (authenticators & Authenticators.BIOMETRIC_WEAK) != 0;
        boolean credential = (authenticators & Authenticators.DEVICE_CREDENTIAL) != 0;
        if (credential) {
            // With or without biometric bits: no sensor is eligible, the credential decides.
            return new int[] {BiometricAuthenticator.TYPE_CREDENTIAL,
                    mHost.isDeviceSecure(userId) ? AUTHENTICATOR_OK : CREDENTIAL_NOT_ENROLLED};
        }
        if (!biometric && authenticators == Authenticators.IDENTITY_CHECK) {
            // Identity Check is never active: Gesher has no setting or Strong biometric for it.
            return new int[] {BiometricAuthenticator.TYPE_NONE,
                    MANDATORY_BIOMETRIC_UNAVAILABLE_ERROR};
        }
        return new int[] {BiometricAuthenticator.TYPE_NONE, BIOMETRIC_NO_HARDWARE};
    }

    /** Utils.authenticatorStatusToBiometricConstant. */
    private static int toBiometricConstant(int status) {
        switch (status) {
            case AUTHENTICATOR_OK:
                return BiometricConstants.BIOMETRIC_SUCCESS;
            case BIOMETRIC_NO_HARDWARE:
                return BiometricConstants.BIOMETRIC_ERROR_HW_NOT_PRESENT;
            case CREDENTIAL_NOT_ENROLLED:
                return BiometricConstants.BIOMETRIC_ERROR_NO_DEVICE_CREDENTIAL;
            case MANDATORY_BIOMETRIC_UNAVAILABLE_ERROR:
                return BiometricConstants.BIOMETRIC_ERROR_IDENTITY_CHECK_NOT_ACTIVE;
            default:
                return BiometricConstants.BIOMETRIC_ERROR_HW_UNAVAILABLE;
        }
    }

    /** Utils.biometricConstantsToBiometricManager, for the constants above. */
    private static int toBiometricManager(int constant) {
        switch (constant) {
            case BiometricConstants.BIOMETRIC_SUCCESS:
                return BiometricManager.BIOMETRIC_SUCCESS;
            case BiometricConstants.BIOMETRIC_ERROR_NO_DEVICE_CREDENTIAL:
                return BiometricManager.BIOMETRIC_ERROR_NONE_ENROLLED;
            case BiometricConstants.BIOMETRIC_ERROR_HW_NOT_PRESENT:
                return BiometricManager.BIOMETRIC_ERROR_NO_HARDWARE;
            case BiometricConstants.BIOMETRIC_ERROR_IDENTITY_CHECK_NOT_ACTIVE:
                return BiometricManager.BIOMETRIC_ERROR_IDENTITY_CHECK_NOT_ACTIVE;
            default:
                return BiometricManager.BIOMETRIC_ERROR_HW_UNAVAILABLE;
        }
    }

    /** Utils.isValidAuthenticatorConfig; invalid is BiometricService's SecurityException. */
    private void validate(int authenticators) {
        if (authenticators == 0) return;
        int allowed = Authenticators.DEVICE_CREDENTIAL | Authenticators.BIOMETRIC_MIN_STRENGTH
                | Authenticators.IDENTITY_CHECK;
        boolean valid;
        if ((authenticators & ~allowed) != 0) {
            valid = false;
        } else {
            int biometric = authenticators & Authenticators.BIOMETRIC_MIN_STRENGTH;
            if (biometric == Authenticators.EMPTY_SET
                    && (authenticators & Authenticators.DEVICE_CREDENTIAL) != 0) {
                valid = true;
            } else if (biometric == Authenticators.BIOMETRIC_STRONG
                    || biometric == Authenticators.BIOMETRIC_WEAK) {
                valid = true;
            } else if ((authenticators & Authenticators.IDENTITY_CHECK) != 0) {
                enforce(SET_BIOMETRIC_DIALOG_ADVANCED);
                valid = true;
            } else {
                valid = false;
            }
        }
        if (!valid) throw new SecurityException("Invalid authenticator configuration");
    }

    /** BiometricManager.Strings with no sensors: the credential's string, or null. */
    private CharSequence credentialString(int userId, int authenticators, String name) {
        checkUser(userId);
        validate(authenticators);
        return (authenticators & Authenticators.DEVICE_CREDENTIAL) != 0 ? mHost.string(name) : null;
    }

    /** What Windows' prompt says: the app's title, subtitle and description. */
    private static String message(PromptInfo info) {
        StringBuilder text = new StringBuilder();
        for (CharSequence part : new CharSequence[] {
                info.getTitle(), info.getSubtitle(), info.getDescription()}) {
            if (part == null || part.length() == 0) continue;
            if (text.length() > 0) text.append('\n');
            text.append(part);
        }
        return text.toString();
    }

    private long fastFail(IBiometricServiceReceiver receiver) {
        if (receiver != null) {
            try {
                receiver.onError(BiometricAuthenticator.TYPE_NONE,
                        BiometricConstants.BIOMETRIC_ERROR_CANCELED, 0);
            } catch (RemoteException e) {
                SystemServer.log("auth: fast fail not delivered: " + e);
            }
        }
        return -1;
    }

    /** AuthService.checkPermission: USE_FINGERPRINT, or else USE_BIOMETRIC. */
    private void checkPermission() {
        if (!mHost.hasPermission(Manifest.permission.USE_FINGERPRINT, Binder.getCallingPid(),
                Binder.getCallingUid())) {
            if (!mHost.hasPermission(Manifest.permission.USE_BIOMETRIC, Binder.getCallingPid(),
                    Binder.getCallingUid())) {
                throw new SecurityException("Must have USE_BIOMETRIC permission");
            }
        }
    }

    /** Another user's request needs USE_BIOMETRIC_INTERNAL, the caller's own the public one. */
    private void checkUser(int userId) {
        if (userId != UserHandle.getCallingUserId()) {
            enforce(USE_BIOMETRIC_INTERNAL);
        } else {
            checkPermission();
        }
    }

    private void enforce(String permission) {
        if (!mHost.hasPermission(permission, Binder.getCallingPid(), Binder.getCallingUid())) {
            throw new SecurityException("Must have " + permission + " permission");
        }
    }
}
