/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URI;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Digital Asset Links for passkeys (docs/passkeys.md): a relying party lets an Android
 * application use its WebAuthn credentials by publishing
 * {@code https://<rpId>/.well-known/assetlinks.json} with a statement that grants
 * {@code delegate_permission/common.get_login_creds} to the application's package and
 * signing certificate (https://developers.google.com/digital-asset-links/v1/getting-started,
 * https://developer.android.com/identity/sign-in/credential-manager#add-support-dal).
 * Android's credential providers check this before any passkey operation; so does
 * {@link CredentialManagerService}. Every doubt is a refusal.
 */
final class AssetLinks {
    private AssetLinks() {
    }

    static final String RELATION = "delegate_permission/common.get_login_creds";

    /** Retrieves a host's statement list. */
    interface Fetcher {
        /**
         * The body of {@code https://<host>/.well-known/assetlinks.json}. Throws when the
         * host does not serve one directly (no redirects) with status 200 as JSON.
         */
        String fetch(String host) throws IOException;
    }

    /**
     * Whether {@code rpId} grants {@link #RELATION} to {@code packageName} signed with one of
     * {@code certificates} (DER). False for an invalid id, a failed fetch, a malformed
     * document or a missing statement.
     */
    static boolean verify(Fetcher fetcher, String rpId, String packageName,
            byte[][] certificates) {
        if (fetcher == null || packageName == null || certificates == null
                || certificates.length == 0 || !PasskeyRequests.validRpId(rpId)) {
            return false;
        }
        String statements;
        try {
            statements = fetcher.fetch(rpId);
        } catch (IOException | RuntimeException failure) {
            System.err.println("gesher credentials: asset links of " + rpId + ": " + failure);
            return false;
        }
        return grants(statements, packageName, certificates);
    }

    /** Whether the statement list grants {@link #RELATION} to the package and a certificate. */
    static boolean grants(String statements, String packageName, byte[][] certificates) {
        List<Object> list;
        try {
            list = PasskeyJson.asArray(PasskeyJson.parse(statements));
        } catch (IllegalArgumentException malformed) {
            return false;
        }
        if (list == null) return false;
        String[] fingerprints = new String[certificates.length];
        for (int i = 0; i < certificates.length; i++) {
            fingerprints[i] = fingerprint(certificates[i]);
        }
        for (Object element : list) {
            Map<String, Object> statement = PasskeyJson.asObject(element);
            if (statement == null) continue;
            List<Object> relations = PasskeyJson.asArray(statement.get("relation"));
            Map<String, Object> target = PasskeyJson.asObject(statement.get("target"));
            if (relations == null || target == null || !relations.contains(RELATION)) continue;
            if (!"android_app".equals(target.get("namespace"))
                    || !packageName.equals(target.get("package_name"))) {
                continue;
            }
            List<Object> granted = PasskeyJson.asArray(target.get("sha256_cert_fingerprints"));
            if (granted == null) continue;
            for (Object value : granted) {
                if (!(value instanceof String)) continue;
                String candidate = ((String) value).toUpperCase(Locale.ROOT);
                for (String fingerprint : fingerprints) {
                    if (fingerprint.equals(candidate)) return true;
                }
            }
        }
        return false;
    }

    /** A certificate's SHA-256 as asset links write it: upper-case hex pairs joined by ':'. */
    static String fingerprint(byte[] certificate) {
        byte[] digest = PasskeyRequests.sha256(certificate);
        StringBuilder out = new StringBuilder(digest.length * 3);
        for (int i = 0; i < digest.length; i++) {
            if (i > 0) out.append(':');
            out.append(Character.toUpperCase(Character.forDigit(digest[i] >> 4 & 0xf, 16)));
            out.append(Character.toUpperCase(Character.forDigit(digest[i] & 0xf, 16)));
        }
        return out.toString();
    }

    /**
     * The production fetcher: an HTTPS GET from the system server through the same stack
     * applications use (docs/network.md: Windows' proxy settings and Windows' trusted
     * roots). Redirects are not followed, as Digital Asset Links require.
     */
    static final class Https implements Fetcher {
        static final int TIMEOUT_MS = 10_000;
        static final int MAX_BYTES = 1 << 20;

        /** Opens the connection for a statement list's URL. */
        interface Connector {
            HttpURLConnection open(URL url) throws IOException;
        }

        private final Connector mConnector;

        Https() {
            this(url -> (HttpURLConnection) url.openConnection());
        }

        /** For host tests, which serve the statement list from a local server. */
        Https(Connector connector) {
            mConnector = connector;
        }

        @Override
        public String fetch(String host) throws IOException {
            if (!PasskeyRequests.validRpId(host)) throw new IOException("not a host name");
            // The host is a validated name, so the URL can only be this host's well-known file.
            HttpURLConnection connection = mConnector.open(
                    URI.create("https://" + host + "/.well-known/assetlinks.json").toURL());
            try {
                connection.setInstanceFollowRedirects(false);
                connection.setConnectTimeout(TIMEOUT_MS);
                connection.setReadTimeout(TIMEOUT_MS);
                connection.setUseCaches(false);
                connection.setRequestMethod("GET");
                connection.setRequestProperty("Accept", "application/json");
                int status = connection.getResponseCode();
                if (status != HttpURLConnection.HTTP_OK) throw new IOException("HTTP " + status);
                String type = connection.getContentType();
                if (type == null
                        || !type.toLowerCase(Locale.ROOT).startsWith("application/json")) {
                    throw new IOException("content type " + type);
                }
                ByteArrayOutputStream body = new ByteArrayOutputStream();
                try (InputStream in = connection.getInputStream()) {
                    byte[] buffer = new byte[8192];
                    int count;
                    while ((count = in.read(buffer)) > 0) {
                        if (body.size() + count > MAX_BYTES) throw new IOException("too large");
                        body.write(buffer, 0, count);
                    }
                }
                return new String(body.toByteArray(), StandardCharsets.UTF_8);
            } finally {
                connection.disconnect();
            }
        }
    }
}
