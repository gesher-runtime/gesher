/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.wifi.WifiEnterpriseConfig;
import java.io.ByteArrayInputStream;
import java.security.KeyFactory;
import java.security.PrivateKey;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.Base64;

/** Public test-only P-384 material reused from the reviewed specifier fixture.
 * Fixed key scalars 560101/560102, validity 2020-01-01 through 2040-01-01.
 * No certificate is installed and no connection or trust decision is attempted.
 */
final class WifiSuggestionCertificates {
    private static final String EC384_CA =
            "MIIBpjCCASygAwIBAgIDCIvlMAoGCCqGSM49BAMDMDExLzAtBgNVBAMMJkdlc2hlciBwdWJsaWMgdGVzdC1vbmx5IFdpLUZpIEVD" +
            "Mzg0IENBMB4XDTIwMDEwMTAwMDAwMFoXDTQwMDEwMTAwMDAwMFowMTEvMC0GA1UEAwwmR2VzaGVyIHB1YmxpYyB0ZXN0LW9ubHkg" +
            "V2ktRmkgRUMzODQgQ0EwdjAQBgcqhkjOPQIBBgUrgQQAIgNiAATYC+dSU8lnLHR34nU44MXFERfKodOts8geH7Lljhx5JCf5KjES" +
            "r+1GluWjalM6CzVtGE2pVuBlVc2EwZtDAXCPT0NWIgBAZtgsQ2+ZyfoBuaSSdo1sN0yrQGXGfFkKFcCjFjAUMBIGA1UdEwEB/wQI" +
            "MAYBAf8CAQEwCgYIKoZIzj0EAwMDaAAwZQIwbKVVmy5BaHbx9aA7b8b/Gs7HhV57mdoUFc80CMMtDtXxDvp6/IKGp7DDWC8HPBee" +
            "AjEAiKTbOYigPeewdGKcYJ14Vw/eQcMkEO7PSIastZapVfeBWRaD/84COpMx+swxWdOL";
    private static final String EC384_CLIENT =
            "MIIBpDCCASqgAwIBAgIDCIvmMAoGCCqGSM49BAMDMDExLzAtBgNVBAMMJkdlc2hlciBwdWJsaWMgdGVzdC1vbmx5IFdpLUZpIEVD" +
            "Mzg0IENBMB4XDTIwMDEwMTAwMDAwMFoXDTQwMDEwMTAwMDAwMFowNTEzMDEGA1UEAwwqR2VzaGVyIHB1YmxpYyB0ZXN0LW9ubHkg" +
            "V2ktRmkgRUMzODQgY2xpZW50MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAEQLQR7e5gMPSzQEvZpCllHVOPkAcRWUSxO7ZElXrbE7pT" +
            "ENYk2hNJwf2odEBGJ/ThhxnO/rJJINMFICjyjUsAtB7tOwDTKvU2BNW76mMpQnyBbTb4tew920vNLSIYrfpRoxAwDjAMBgNVHRMB" +
            "Af8EAjAAMAoGCCqGSM49BAMDA2gAMGUCMBro6olOVAnO4ahWv0lZz/lpqotAn4TJLo3YzE4QzwtwBkpregw3tGWLk6jeGrx/ZQIx" +
            "AJAnqejDcZOG/e9DGG7I2F/fWoy11ty8kpSIw+qnGVAdnFPEqewAHhNc4eAgJXCy6w==";
    private static final String EC384_KEY =
            "MIG2AgEAMBAGByqGSM49AgEGBSuBBAAiBIGeMIGbAgEBBDAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
            "AAAAAAAIi+ahZANiAARAtBHt7mAw9LNAS9mkKWUdU4+QBxFZRLE7tkSVetsTulMQ1iTaE0nB/ah0QEYn9OGHGc7+skkg0wUgKPKN" +
            "SwC0Hu07ANMq9TYE1bvqYylCfIFtNvi17D3bS80tIhit+lE=";

    static X509Certificate ca() throws Exception { return certificate(EC384_CA); }
    static X509Certificate client() throws Exception { return certificate(EC384_CLIENT); }
    private static X509Certificate certificate(String encoded) throws Exception {
        return (X509Certificate) CertificateFactory.getInstance("X.509").generateCertificate(
                new ByteArrayInputStream(Base64.getDecoder().decode(encoded)));
    }
    static PrivateKey key() throws Exception {
        return KeyFactory.getInstance("EC").generatePrivate(
                new PKCS8EncodedKeySpec(Base64.getDecoder().decode(EC384_KEY)));
    }
    static WifiEnterpriseConfig enterprise(boolean tls) throws Exception {
        WifiEnterpriseConfig value = new WifiEnterpriseConfig();
        value.setEapMethod(tls ? WifiEnterpriseConfig.Eap.TLS : WifiEnterpriseConfig.Eap.TTLS);
        value.setPhase2Method(tls ? WifiEnterpriseConfig.Phase2.NONE : WifiEnterpriseConfig.Phase2.PAP);
        value.setIdentity("suggestion-public-test");
        value.setPassword("suggestion-public-test-password");
        value.setDomainSuffixMatch("wifi.example.test");
        value.setCaCertificate(ca());
        if (tls) value.setClientKeyEntry(key(), client());
        return value;
    }
}
