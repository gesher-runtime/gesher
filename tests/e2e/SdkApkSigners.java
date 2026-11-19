/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */

import com.android.apksig.ApkVerifier;
import java.io.File;
import java.security.MessageDigest;
import java.security.cert.X509Certificate;
import java.util.HexFormat;
import java.util.List;

/** Source-launched SDK adapter. Inspects verified APK bytes; never loads APK code. */
public final class SdkApkSigners {
    private static String digest(X509Certificate certificate) throws Exception {
        return HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256")
                .digest(certificate.getEncoded()));
    }

    private static void ranges(StringBuilder out, String scheme,
            List<ApkVerifier.Result.V3SchemeSignerInfo> signers, boolean[] separator)
            throws Exception {
        for (var signer : signers) {
            if (separator[0]) out.append(',');
            separator[0] = true;
            out.append("{\"scheme\":\"").append(scheme)
                    .append("\",\"min_sdk\":").append(signer.getMinSdkVersion())
                    .append(",\"max_sdk\":").append(signer.getMaxSdkVersion())
                    .append(",\"rotation_targets_dev_release\":")
                    .append(signer.getRotationTargetsDevRelease())
                    .append(",\"certificate_sha256\":\"")
                    .append(digest(signer.getCertificate())).append("\"}");
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 1) throw new IllegalArgumentException("Expected one APK path");
        // This is the same default supported-platform verification used by the
        // apksigner CLI. Result.getSignerCertificates owns current-set selection;
        // API-range records are separate observations, not concurrent signers.
        ApkVerifier.Result result = new ApkVerifier.Builder(new File(args[0])).build().verify();
        if (!result.isVerified() || result.getSignerCertificates().isEmpty()) {
            System.err.println("APK verification failed");
            System.exit(1);
            return;
        }
        StringBuilder out = new StringBuilder("{\"signer_certificate_sha256\":[");
        boolean separator = false;
        for (X509Certificate certificate : result.getSignerCertificates()) {
            if (separator) out.append(',');
            separator = true;
            out.append('"').append(digest(certificate)).append('"');
        }
        out.append("],\"source_stamp_certificate_sha256\":");
        var stamp = result.getSourceStampInfo();
        if (stamp != null && stamp.getCertificate() != null) {
            out.append('"').append(digest(stamp.getCertificate())).append('"');
        } else {
            out.append("null");
        }
        out.append(",\"signer_api_ranges\":[");
        boolean[] rangeSeparator = {false};
        if (result.isVerifiedUsingV31Scheme()) {
            ranges(out, "v3.1", result.getV31SchemeSigners(), rangeSeparator);
        }
        if (result.isVerifiedUsingV3Scheme()) {
            ranges(out, "v3", result.getV3SchemeSigners(), rangeSeparator);
        }
        System.out.println(out.append("]}").toString());
    }
}
