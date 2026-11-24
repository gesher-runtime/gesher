/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.os.flagging;

/**
 * The ConfigInfrastructure module's exception for a failed read of aconfig flag storage
 * (see {@link AconfigPackage}), with the module's public error codes.
 */
public class AconfigStorageReadException extends RuntimeException {
    public static final int ERROR_GENERIC = 0;
    public static final int ERROR_STORAGE_SYSTEM_NOT_FOUND = 1;
    public static final int ERROR_PACKAGE_NOT_FOUND = 2;
    public static final int ERROR_CONTAINER_NOT_FOUND = 3;
    public static final int ERROR_CANNOT_READ_STORAGE_FILE = 4;
    /** @hide */
    public static final int ERROR_FILE_FINGERPRINT_MISMATCH = 5;

    private final int mErrorCode;

    public AconfigStorageReadException(int errorCode, String msg) {
        super(msg);
        mErrorCode = errorCode;
    }

    public AconfigStorageReadException(int errorCode, String msg, Throwable cause) {
        super(msg, cause);
        mErrorCode = errorCode;
    }

    public AconfigStorageReadException(int errorCode, Throwable cause) {
        super(cause);
        mErrorCode = errorCode;
    }

    public int getErrorCode() {
        return mErrorCode;
    }

    @Override
    public String getMessage() {
        return errorString() + ": " + super.getMessage();
    }

    private String errorString() {
        switch (mErrorCode) {
            case ERROR_GENERIC:
                return "ERROR_GENERIC";
            case ERROR_STORAGE_SYSTEM_NOT_FOUND:
                return "ERROR_STORAGE_SYSTEM_NOT_FOUND";
            case ERROR_PACKAGE_NOT_FOUND:
                return "ERROR_PACKAGE_NOT_FOUND";
            case ERROR_CONTAINER_NOT_FOUND:
                return "ERROR_CONTAINER_NOT_FOUND";
            case ERROR_CANNOT_READ_STORAGE_FILE:
                return "ERROR_CANNOT_READ_STORAGE_FILE";
            case ERROR_FILE_FINGERPRINT_MISMATCH:
                return "ERROR_FILE_FINGERPRINT_MISMATCH";
            default:
                return "<Unknown error code " + mErrorCode + ">";
        }
    }
}
