/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.android.org.conscrypt.metrics;

/**
 * Gesher: a stand-in for the ConscryptStatsLog that Soong generates with
 * stats-log-api-gen (Conscrypt's conscrypt-statslog-java-gen), which writes
 * Conscrypt's atoms to statsd. Gesher has no statsd: the atoms' constants are
 * here, distinct within each atom, and writing an atom does nothing.
 * (docs/network.md)
 */
public final class ConscryptStatsLog {
    private ConscryptStatsLog() {
    }

    // Atom ids, as in frameworks/proto_logging/stats/atoms.proto.
    public static final int TLS_HANDSHAKE_REPORTED = 317;
    public static final int CERTIFICATE_TRANSPARENCY_LOG_LIST_STATE_CHANGED = 934;
    public static final int CERTIFICATE_TRANSPARENCY_VERIFICATION_REPORTED = 989;
    public static final int CERTIFICATE_BLOCKLIST_BLOCK_REPORTED = 1143;

    public static final int TLS_HANDSHAKE_REPORTED__SOURCE__SOURCE_UNKNOWN = 0;
    public static final int TLS_HANDSHAKE_REPORTED__SOURCE__SOURCE_MAINLINE = 1;
    public static final int TLS_HANDSHAKE_REPORTED__SOURCE__SOURCE_GMS = 2;
    public static final int TLS_HANDSHAKE_REPORTED__SOURCE__SOURCE_UNBUNDLED = 3;

    public static final int CERTIFICATE_TRANSPARENCY_LOG_LIST_STATE_CHANGED__STATUS__STATUS_UNKNOWN = 0;
    public static final int CERTIFICATE_TRANSPARENCY_LOG_LIST_STATE_CHANGED__STATUS__STATUS_SUCCESS = 1;
    public static final int CERTIFICATE_TRANSPARENCY_LOG_LIST_STATE_CHANGED__STATUS__STATUS_NOT_FOUND = 2;
    public static final int CERTIFICATE_TRANSPARENCY_LOG_LIST_STATE_CHANGED__STATUS__STATUS_PARSING_FAILED = 3;
    public static final int CERTIFICATE_TRANSPARENCY_LOG_LIST_STATE_CHANGED__STATUS__STATUS_EXPIRED = 4;

    public static final int CERTIFICATE_TRANSPARENCY_VERIFICATION_REPORTED__RESULT__RESULT_UNKNOWN = 0;
    public static final int CERTIFICATE_TRANSPARENCY_VERIFICATION_REPORTED__RESULT__RESULT_SUCCESS = 1;
    public static final int CERTIFICATE_TRANSPARENCY_VERIFICATION_REPORTED__RESULT__RESULT_FAILURE_NO_SCTS_FOUND = 2;
    public static final int CERTIFICATE_TRANSPARENCY_VERIFICATION_REPORTED__RESULT__RESULT_FAILURE_SCTS_NOT_COMPLIANT = 3;
    public static final int CERTIFICATE_TRANSPARENCY_VERIFICATION_REPORTED__RESULT__RESULT_FAIL_OPEN_NO_LOG_LIST_AVAILABLE = 4;
    public static final int CERTIFICATE_TRANSPARENCY_VERIFICATION_REPORTED__RESULT__RESULT_FAIL_OPEN_LOG_LIST_NOT_COMPLIANT = 5;

    public static final int CERTIFICATE_TRANSPARENCY_VERIFICATION_REPORTED__REASON__REASON_UNKNOWN = 0;
    public static final int CERTIFICATE_TRANSPARENCY_VERIFICATION_REPORTED__REASON__REASON_DRY_RUN = 1;
    public static final int CERTIFICATE_TRANSPARENCY_VERIFICATION_REPORTED__REASON__REASON_SDK_TARGET_DEFAULT_ENABLED = 2;
    public static final int CERTIFICATE_TRANSPARENCY_VERIFICATION_REPORTED__REASON__REASON_NSCONFIG_APP_OPT_IN = 3;
    public static final int CERTIFICATE_TRANSPARENCY_VERIFICATION_REPORTED__REASON__REASON_NSCONFIG_DOMAIN_OPT_IN = 4;

    public static final int CERTIFICATE_BLOCKLIST_BLOCK_REPORTED__SOURCE__BLOCKLIST_SOURCE_UNKNOWN = 0;
    public static final int CERTIFICATE_BLOCKLIST_BLOCK_REPORTED__SOURCE__BLOCKLIST_SOURCE_SHA1_BUILT_IN = 1;
    public static final int CERTIFICATE_BLOCKLIST_BLOCK_REPORTED__SOURCE__BLOCKLIST_SOURCE_SHA256_BUILT_IN = 2;
    public static final int CERTIFICATE_BLOCKLIST_BLOCK_REPORTED__SOURCE__BLOCKLIST_SOURCE_SHA1_FILE = 3;
    public static final int CERTIFICATE_BLOCKLIST_BLOCK_REPORTED__SOURCE__BLOCKLIST_SOURCE_SHA256_FILE = 4;
    public static final int CERTIFICATE_BLOCKLIST_BLOCK_REPORTED__SOURCE__BLOCKLIST_SOURCE_SHA1_TEST = 5;
    public static final int CERTIFICATE_BLOCKLIST_BLOCK_REPORTED__SOURCE__BLOCKLIST_SOURCE_SHA256_TEST = 6;

    /** TLS_HANDSHAKE_REPORTED. */
    public static void write(int code, boolean arg1, int arg2, int arg3, int arg4, int arg5,
            int[] arg6) {
    }

    /** CERTIFICATE_TRANSPARENCY_LOG_LIST_STATE_CHANGED. */
    public static void write(int code, int arg1, int arg2, int arg3, int arg4, int arg5) {
    }

    /** CERTIFICATE_TRANSPARENCY_VERIFICATION_REPORTED. */
    public static void write(int code, int arg1, int arg2, int arg3, int arg4, int arg5, int arg6,
            int arg7, int arg8, int arg9) {
    }

    /** CERTIFICATE_BLOCKLIST_BLOCK_REPORTED. */
    public static void write(int code, int arg1, int arg2, int arg3) {
    }
}
