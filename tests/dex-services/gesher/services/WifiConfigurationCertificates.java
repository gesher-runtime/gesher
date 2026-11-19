/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import java.io.ByteArrayInputStream;
import java.security.KeyFactory;
import java.security.PrivateKey;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.Base64;

/** Public test-only P-256 material, never an installed trust anchor or authentication credential.
 * Generated for this fixture with cryptography 50.0.0: CA/client private scalars 560001/560002,
 * matching serials, 2020-01-01..2040-01-01, BasicConstraints CA(pathLen=1)/end entity, ECDSA-SHA256.
 */
final class WifiConfigurationCertificates {
    private static final String CA =
            "MIIBTjCB9aADAgECAgMIi4EwCgYIKoZIzj0EAwIwJDEiMCAGA1UEAwwZR2VzaGVyIHRlc3Qtb25seSBXaS1GaSBDQTAeFw0yMDAx" +
            "MDEwMDAwMDBaFw00MDAxMDEwMDAwMDBaMCQxIjAgBgNVBAMMGUdlc2hlciB0ZXN0LW9ubHkgV2ktRmkgQ0EwWTATBgcqhkjOPQIB" +
            "BggqhkjOPQMBBwNCAARqEfo6k0+oQ0XeFieEDSt+ofoMIf5A3yWUJvXHpHZx4Wok9oaZ9XnmBq7l5llFQVYv+WdRZq6CetdcNpD4" +
            "B2cKoxYwFDASBgNVHRMBAf8ECDAGAQH/AgEBMAoGCCqGSM49BAMCA0gAMEUCIQCRS9tZVQneccRDdJ9yinU65GfVUE161hHD2ptx" +
            "A/UYAQIgSxBjaOe9PvLadm+59IVm+e+PaNM/2FQ48UYYxikp95Y=";
    private static final String CLIENT =
            "MIIBTDCB86ADAgECAgMIi4IwCgYIKoZIzj0EAwIwJDEiMCAGA1UEAwwZR2VzaGVyIHRlc3Qtb25seSBXaS1GaSBDQTAeFw0yMDAx" +
            "MDEwMDAwMDBaFw00MDAxMDEwMDAwMDBaMCgxJjAkBgNVBAMMHUdlc2hlciB0ZXN0LW9ubHkgV2ktRmkgY2xpZW50MFkwEwYHKoZI" +
            "zj0CAQYIKoZIzj0DAQcDQgAExWuLnB0EBEySniV2yQwE9J7Q4+sen7ueOaQtf4wIJ89YnDTLFCo1gIZaeIy4ykf2jixnrT0iVe/8" +
            "zYWuU/kY7KMQMA4wDAYDVR0TAQH/BAIwADAKBggqhkjOPQQDAgNIADBFAiEA0cq1O/AdLriydT7E4DjT2+X8c7L9jwOfr9/CNsCr" +
            "nl8CIGDsEgv8ZTGu7aScUXbsQaeCWoJE0pbO2Szx2nLYUxWi";
    private static final String KEY =
            "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIi4KhRANCAATF" +
            "a4ucHQQETJKeJXbJDAT0ntDj6x6fu545pC1/jAgnz1icNMsUKjWAhlp4jLjKR/aOLGetPSJV7/zNha5T+Rjs";
    private static X509Certificate certificate(String encoded) throws Exception {
        return (X509Certificate) CertificateFactory.getInstance("X.509").generateCertificate(
                new ByteArrayInputStream(Base64.getDecoder().decode(encoded)));
    }
    static X509Certificate ca() throws Exception { return certificate(CA); }
    static X509Certificate client() throws Exception { return certificate(CLIENT); }
    static PrivateKey key() throws Exception {
        return KeyFactory.getInstance("EC").generatePrivate(
                new PKCS8EncodedKeySpec(Base64.getDecoder().decode(KEY)));
    }
}
