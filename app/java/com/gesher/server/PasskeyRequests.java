/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Base64;
import java.util.List;
import java.util.Map;
import java.util.TreeSet;

/**
 * The WebAuthn JSON of the passkey bridge (docs/passkeys.md): an application's
 * PublicKeyCredentialCreationOptionsJSON / PublicKeyCredentialRequestOptionsJSON become
 * {@link PasskeyBackend} requests, Gesher builds clientDataJSON with the application's own
 * Android origin, and what Windows returned becomes a RegistrationResponseJSON /
 * AuthenticationResponseJSON (https://www.w3.org/TR/webauthn-3/#sctn-parseCreationOptionsFromJSON,
 * #dictdef-registrationresponsejson, #dictdef-authenticationresponsejson). Every response
 * member is either the application's own input or bytes Windows returned.
 */
final class PasskeyRequests {
    private PasskeyRequests() {
    }

    /** DOMException names used for requests Gesher refuses before asking Windows. */
    static final String DATA_ERROR = "DataError";
    static final String NOT_SUPPORTED_ERROR = "NotSupportedError";
    static final String SECURITY_ERROR = "SecurityError";

    /** The client's time-out when the relying party gives none, and the range it honors. */
    static final int DEFAULT_TIMEOUT_MS = 300_000;
    static final int MIN_TIMEOUT_MS = 10_000;
    static final int MAX_TIMEOUT_MS = 600_000;

    /** A request that cannot be performed, as the DOMException a WebAuthn client raises. */
    static final class Refusal extends Exception {
        private static final long serialVersionUID = 1;
        final String domError;

        Refusal(String domError, String message) {
            super(message);
            this.domError = domError;
        }
    }

    /** A parsed creation request; {@code request} lacks only pid and clientDataJson. */
    static final class Creation {
        final PasskeyBackend.MakeCredential request = new PasskeyBackend.MakeCredential();
        byte[] challenge;
        /** The credProps extension was requested. */
        boolean credProps;
    }

    /** A parsed assertion request; {@code request} lacks only pid and clientDataJson. */
    static final class Authentication {
        final PasskeyBackend.GetAssertion request = new PasskeyBackend.GetAssertion();
        byte[] challenge;
    }

    static Creation parseCreation(String json) throws Refusal {
        Map<String, Object> options = object(json);
        Creation creation = new Creation();
        PasskeyBackend.MakeCredential request = creation.request;

        Map<String, Object> rp = requiredObject(options, "rp");
        // A web page may leave rp.id to default to its origin's domain. An Android
        // application has no such domain, so the relying party must name itself.
        request.rpId = rpId(requiredString(rp, "id"));
        request.rpName = requiredString(rp, "name");

        Map<String, Object> user = requiredObject(options, "user");
        request.userId = bytes(requiredString(user, "id"), "user.id");
        if (request.userId.length < 1 || request.userId.length > 64) {
            throw new Refusal(DATA_ERROR, "user.id must be 1 to 64 bytes");
        }
        request.userName = requiredString(user, "name");
        request.userDisplayName = requiredString(user, "displayName");

        creation.challenge = challenge(options);

        List<Object> params = PasskeyJson.asArray(options.get("pubKeyCredParams"));
        if (params == null) throw new Refusal(DATA_ERROR, "pubKeyCredParams is required");
        List<Integer> algorithms = new ArrayList<>();
        for (Object element : params) {
            Map<String, Object> param = PasskeyJson.asObject(element);
            if (param == null) throw new Refusal(DATA_ERROR, "pubKeyCredParams element");
            // Unknown credential types are skipped, as the specification's client does.
            if (!"public-key".equals(param.get("type"))) continue;
            algorithms.add(integer(param.get("alg"), "pubKeyCredParams.alg"));
        }
        if (algorithms.isEmpty()) {
            throw new Refusal(NOT_SUPPORTED_ERROR, "no public-key credential parameters");
        }
        request.algorithms = new int[algorithms.size()];
        for (int i = 0; i < request.algorithms.length; i++) {
            request.algorithms[i] = algorithms.get(i);
        }

        request.timeoutMs = timeout(options);
        request.exclude = descriptors(options.get("excludeCredentials"), "excludeCredentials");

        Object selectionValue = options.get("authenticatorSelection");
        if (present(selectionValue)) {
            Map<String, Object> selection = PasskeyJson.asObject(selectionValue);
            if (selection == null) throw new Refusal(DATA_ERROR, "authenticatorSelection");
            String attachment = optionalString(selection, "authenticatorAttachment");
            if ("platform".equals(attachment)) {
                request.attachment = PasskeyBackend.ATTACHMENT_PLATFORM;
            } else if ("cross-platform".equals(attachment)) {
                request.attachment = PasskeyBackend.ATTACHMENT_CROSS_PLATFORM;
            }
            String residentKey = optionalString(selection, "residentKey");
            if ("required".equals(residentKey)) {
                request.residentKey = PasskeyBackend.RESIDENT_KEY_REQUIRED;
            } else if ("preferred".equals(residentKey)) {
                request.residentKey = PasskeyBackend.RESIDENT_KEY_PREFERRED;
            } else if ("discouraged".equals(residentKey)) {
                request.residentKey = PasskeyBackend.RESIDENT_KEY_DISCOURAGED;
            } else if (Boolean.TRUE.equals(selection.get("requireResidentKey"))) {
                // Without a known residentKey, the older member decides.
                request.residentKey = PasskeyBackend.RESIDENT_KEY_REQUIRED;
            }
            request.userVerification =
                    userVerification(optionalString(selection, "userVerification"));
        }

        String attestation = optionalString(options, "attestation");
        if ("indirect".equals(attestation)) {
            request.attestation = PasskeyBackend.ATTESTATION_INDIRECT;
        } else if ("direct".equals(attestation)) {
            request.attestation = PasskeyBackend.ATTESTATION_DIRECT;
        }  // "none", "enterprise" (not offered here) and unknown values: none

        Object extensions = options.get("extensions");
        if (present(extensions)) {
            Map<String, Object> members = PasskeyJson.asObject(extensions);
            if (members == null) throw new Refusal(DATA_ERROR, "extensions");
            creation.credProps = Boolean.TRUE.equals(members.get("credProps"));
        }
        return creation;
    }

    static Authentication parseAuthentication(String json) throws Refusal {
        Map<String, Object> options = object(json);
        Authentication authentication = new Authentication();
        PasskeyBackend.GetAssertion request = authentication.request;
        request.rpId = rpId(requiredString(options, "rpId"));
        authentication.challenge = challenge(options);
        request.timeoutMs = timeout(options);
        request.allow = descriptors(options.get("allowCredentials"), "allowCredentials");
        request.userVerification = userVerification(optionalString(options, "userVerification"));
        return authentication;
    }

    /**
     * Whether {@code id} can be a relying party id: a lower-case host name of letters, digits
     * and inner hyphens, with no scheme, port, path, user info or trailing dot. Anything else
     * could direct the asset-links request somewhere other than the named host.
     */
    static boolean validRpId(String id) {
        if (id == null || id.isEmpty() || id.length() > 253) return false;
        int labelStart = 0;
        for (int i = 0; i <= id.length(); i++) {
            if (i < id.length() && id.charAt(i) != '.') {
                char c = id.charAt(i);
                boolean ok = c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '-';
                if (!ok) return false;
                continue;
            }
            int length = i - labelStart;
            if (length < 1 || length > 63) return false;
            if (id.charAt(labelStart) == '-' || id.charAt(i - 1) == '-') return false;
            labelStart = i + 1;
        }
        return true;
    }

    /**
     * The WebAuthn origin of an Android application, as Android's credential providers
     * derive it: {@code android:apk-key-hash:} and the unpadded base64url SHA-256 of its
     * signing certificate.
     */
    static String origin(byte[] signingCertificate) {
        return "android:apk-key-hash:" + base64Url(sha256(signingCertificate));
    }

    /**
     * clientDataJSON as the specification serializes it
     * (https://www.w3.org/TR/webauthn-3/#clientdatajson-serialization): type, challenge,
     * origin, crossOrigin, in that order. {@code type} is webauthn.create or webauthn.get.
     */
    static byte[] clientData(String type, byte[] challenge, String origin) {
        StringBuilder json = new StringBuilder("{\"type\":");
        PasskeyJson.quote(json, type).append(",\"challenge\":");
        PasskeyJson.quote(json, base64Url(challenge)).append(",\"origin\":");
        PasskeyJson.quote(json, origin).append(",\"crossOrigin\":false}");
        return json.toString().getBytes(StandardCharsets.UTF_8);
    }

    /**
     * RegistrationResponseJSON from Windows' attestation. Throws IllegalArgumentException
     * when the authenticator data Windows returned holds no readable credential public key:
     * no response is made up in its place.
     */
    static String registrationResponse(byte[] clientData, PasskeyBackend.Attestation attestation,
            boolean credProps) {
        CredentialKey key = CredentialKey.of(attestation.authenticatorData);
        StringBuilder json = new StringBuilder();
        head(json, attestation.credentialId, attestation.usedTransport);
        json.append(",\"response\":{\"clientDataJSON\":");
        PasskeyJson.quote(json, base64Url(clientData)).append(",\"authenticatorData\":");
        PasskeyJson.quote(json, base64Url(attestation.authenticatorData));
        json.append(",\"transports\":[");
        int transports = attestation.transports >= 0 ? attestation.transports
                : attestation.usedTransport;
        boolean first = true;
        for (String transport : transportNames(transports)) {
            if (!first) json.append(',');
            PasskeyJson.quote(json, transport);
            first = false;
        }
        json.append("]");
        if (key.spki != null) {
            json.append(",\"publicKey\":");
            PasskeyJson.quote(json, base64Url(key.spki));
        }
        json.append(",\"publicKeyAlgorithm\":").append(key.algorithm);
        json.append(",\"attestationObject\":");
        PasskeyJson.quote(json, base64Url(attestation.attestationObject));
        json.append("},\"clientExtensionResults\":{");
        if (credProps) {
            json.append("\"credProps\":{");
            // rk is reported only when Windows said whether the credential is discoverable.
            if (attestation.residentKey >= 0) {
                json.append("\"rk\":").append(attestation.residentKey != 0);
            }
            json.append('}');
        }
        return json.append("}}").toString();
    }

    /** AuthenticationResponseJSON from Windows' assertion. */
    static String authenticationResponse(byte[] clientData, PasskeyBackend.Assertion assertion) {
        StringBuilder json = new StringBuilder();
        head(json, assertion.credentialId, assertion.usedTransport);
        json.append(",\"response\":{\"clientDataJSON\":");
        PasskeyJson.quote(json, base64Url(clientData)).append(",\"authenticatorData\":");
        PasskeyJson.quote(json, base64Url(assertion.authenticatorData)).append(",\"signature\":");
        PasskeyJson.quote(json, base64Url(assertion.signature));
        if (assertion.userId != null && assertion.userId.length > 0) {
            json.append(",\"userHandle\":");
            PasskeyJson.quote(json, base64Url(assertion.userId));
        }
        return json.append("},\"clientExtensionResults\":{}}").toString();
    }

    private static void head(StringBuilder json, byte[] credentialId, int usedTransport) {
        String id = base64Url(credentialId);
        json.append("{\"id\":");
        PasskeyJson.quote(json, id).append(",\"rawId\":");
        PasskeyJson.quote(json, id).append(",\"type\":\"public-key\"");
        // The attachment is known only from the transport Windows says was used.
        if (usedTransport == PasskeyBackend.TRANSPORT_INTERNAL) {
            json.append(",\"authenticatorAttachment\":\"platform\"");
        } else if (usedTransport != 0) {
            json.append(",\"authenticatorAttachment\":\"cross-platform\"");
        }
    }

    /** WEBAUTHN_CTAP_TRANSPORT_* bits as WebAuthn's transport names, sorted, no duplicates. */
    static List<String> transportNames(int bits) {
        TreeSet<String> names = new TreeSet<>();
        if ((bits & PasskeyBackend.TRANSPORT_USB) != 0) names.add("usb");
        if ((bits & PasskeyBackend.TRANSPORT_NFC) != 0) names.add("nfc");
        if ((bits & PasskeyBackend.TRANSPORT_BLE) != 0) names.add("ble");
        if ((bits & PasskeyBackend.TRANSPORT_INTERNAL) != 0) names.add("internal");
        if ((bits & PasskeyBackend.TRANSPORT_HYBRID) != 0) names.add("hybrid");
        if ((bits & PasskeyBackend.TRANSPORT_SMART_CARD) != 0) names.add("smart-card");
        return new ArrayList<>(names);
    }

    private static int transportBit(String name) {
        switch (name) {
            case "usb": return PasskeyBackend.TRANSPORT_USB;
            case "nfc": return PasskeyBackend.TRANSPORT_NFC;
            case "ble": return PasskeyBackend.TRANSPORT_BLE;
            case "internal": return PasskeyBackend.TRANSPORT_INTERNAL;
            case "hybrid": return PasskeyBackend.TRANSPORT_HYBRID;
            case "smart-card": return PasskeyBackend.TRANSPORT_SMART_CARD;
            default: return 0;  // unknown transports are ignored
        }
    }

    static String base64Url(byte[] bytes) {
        return Base64.getUrlEncoder().withoutPadding().encodeToString(bytes);
    }

    static byte[] sha256(byte[] bytes) {
        try {
            return MessageDigest.getInstance("SHA-256").digest(bytes);
        } catch (NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
    }

    private static Map<String, Object> object(String json) throws Refusal {
        try {
            return PasskeyJson.parseObject(json);
        } catch (IllegalArgumentException invalid) {
            throw new Refusal(DATA_ERROR, "The request is not valid JSON: " + invalid.getMessage());
        }
    }

    private static boolean present(Object value) {
        return value != null && value != PasskeyJson.NULL;
    }

    private static Map<String, Object> requiredObject(Map<String, Object> parent, String name)
            throws Refusal {
        Map<String, Object> object = PasskeyJson.asObject(parent.get(name));
        if (object == null) throw new Refusal(DATA_ERROR, name + " is required");
        return object;
    }

    private static String requiredString(Map<String, Object> parent, String name)
            throws Refusal {
        Object value = parent.get(name);
        if (!(value instanceof String)) throw new Refusal(DATA_ERROR, name + " is required");
        return (String) value;
    }

    /** A string member, or null when it is absent, null, or not a string. */
    private static String optionalString(Map<String, Object> parent, String name) {
        Object value = parent.get(name);
        return value instanceof String ? (String) value : null;
    }

    private static String rpId(String id) throws Refusal {
        if (!validRpId(id)) {
            throw new Refusal(SECURITY_ERROR, "The relying party id is not a valid domain");
        }
        return id;
    }

    private static byte[] challenge(Map<String, Object> options) throws Refusal {
        byte[] challenge = bytes(requiredString(options, "challenge"), "challenge");
        if (challenge.length == 0) throw new Refusal(DATA_ERROR, "challenge is empty");
        return challenge;
    }

    private static byte[] bytes(String base64Url, String name) throws Refusal {
        try {
            return Base64.getUrlDecoder().decode(base64Url);
        } catch (IllegalArgumentException invalid) {
            throw new Refusal(DATA_ERROR, name + " is not base64url");
        }
    }

    private static int integer(Object value, String name) throws Refusal {
        if (value instanceof Double) {
            double number = (Double) value;
            if (number == Math.rint(number) && Math.abs(number) <= Integer.MAX_VALUE) {
                return (int) number;
            }
        }
        throw new Refusal(DATA_ERROR, name + " is not an integer");
    }

    /**
     * The ceremony's time-out. The specification lets the client override the relying
     * party's hint; Gesher keeps it within MIN_TIMEOUT_MS..MAX_TIMEOUT_MS.
     */
    private static int timeout(Map<String, Object> options) throws Refusal {
        Object value = options.get("timeout");
        if (!present(value)) return DEFAULT_TIMEOUT_MS;
        if (!(value instanceof Double) || ((Double) value).isNaN() || (Double) value < 0) {
            throw new Refusal(DATA_ERROR, "timeout is not an unsigned number");
        }
        double ms = (Double) value;
        return (int) Math.max(MIN_TIMEOUT_MS, Math.min(MAX_TIMEOUT_MS, ms));
    }

    private static int userVerification(String value) {
        if ("required".equals(value)) return PasskeyBackend.UV_REQUIRED;
        if ("discouraged".equals(value)) return PasskeyBackend.UV_DISCOURAGED;
        return PasskeyBackend.UV_PREFERRED;  // the default, and unknown values
    }

    private static PasskeyBackend.Descriptor[] descriptors(Object value, String name)
            throws Refusal {
        if (!present(value)) return new PasskeyBackend.Descriptor[0];
        List<Object> elements = PasskeyJson.asArray(value);
        if (elements == null) throw new Refusal(DATA_ERROR, name + " is not a list");
        List<PasskeyBackend.Descriptor> descriptors = new ArrayList<>();
        for (Object element : elements) {
            Map<String, Object> descriptor = PasskeyJson.asObject(element);
            if (descriptor == null) throw new Refusal(DATA_ERROR, name + " element");
            if (!"public-key".equals(descriptor.get("type"))) continue;
            byte[] id = bytes(requiredString(descriptor, "id"), name + ".id");
            int transports = 0;
            List<Object> names = PasskeyJson.asArray(descriptor.get("transports"));
            if (names != null) {
                for (Object transport : names) {
                    if (transport instanceof String) transports |= transportBit((String) transport);
                }
            }
            descriptors.add(new PasskeyBackend.Descriptor(id, transports));
        }
        return descriptors.toArray(new PasskeyBackend.Descriptor[0]);
    }

    /**
     * The credential public key inside authenticator data
     * (https://www.w3.org/TR/webauthn-3/#sctn-attested-credential-data): its COSE algorithm
     * and, for the key types read here (EC2 P-256 and RSA), its SubjectPublicKeyInfo.
     */
    static final class CredentialKey {
        final int algorithm;
        /** DER SubjectPublicKeyInfo, or null for a key type this client does not encode. */
        final byte[] spki;

        private CredentialKey(int algorithm, byte[] spki) {
            this.algorithm = algorithm;
            this.spki = spki;
        }

        private static final int FLAG_ATTESTED_CREDENTIAL_DATA = 0x40;
        private static final byte[] P256_SPKI_PREFIX = {
            0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, (byte) 0x86, 0x48, (byte) 0xce, 0x3d, 0x02,
            0x01, 0x06, 0x08, 0x2a, (byte) 0x86, 0x48, (byte) 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03,
            0x42, 0x00, 0x04};
        private static final byte[] RSA_ALGORITHM = {
            0x30, 0x0d, 0x06, 0x09, 0x2a, (byte) 0x86, 0x48, (byte) 0x86, (byte) 0xf7, 0x0d, 0x01,
            0x01, 0x01, 0x05, 0x00};

        static CredentialKey of(byte[] authenticatorData) {
            byte[] data = authenticatorData;
            // rpIdHash 32, flags 1, signCount 4, aaguid 16, credentialIdLength 2.
            if (data == null || data.length < 55 || (data[32] & FLAG_ATTESTED_CREDENTIAL_DATA) == 0) {
                throw new IllegalArgumentException("no attested credential data");
            }
            int idLength = (data[53] & 0xff) << 8 | data[54] & 0xff;
            Cbor cbor = new Cbor(data, 55 + idLength);
            Object value = cbor.value(0);
            if (!(value instanceof Map)) throw new IllegalArgumentException("COSE key");
            Map<?, ?> key = (Map<?, ?>) value;
            Object algorithm = key.get(3L);
            if (!(algorithm instanceof Long) || (Long) algorithm != (int) (long) (Long) algorithm) {
                throw new IllegalArgumentException("COSE key algorithm");
            }
            Object type = key.get(1L);
            byte[] spki = null;
            if (Long.valueOf(2).equals(type) && Long.valueOf(1).equals(key.get(-1L))) {
                Object x = key.get(-2L), y = key.get(-3L);
                if (x instanceof byte[] && y instanceof byte[] && ((byte[]) x).length == 32
                        && ((byte[]) y).length == 32) {
                    ByteArrayOutputStream out = new ByteArrayOutputStream();
                    out.write(P256_SPKI_PREFIX, 0, P256_SPKI_PREFIX.length);
                    out.write((byte[]) x, 0, 32);
                    out.write((byte[]) y, 0, 32);
                    spki = out.toByteArray();
                }
            } else if (Long.valueOf(3).equals(type)) {
                Object n = key.get(-1L), e = key.get(-2L);
                if (n instanceof byte[] && e instanceof byte[]) {
                    byte[] rsaKey = der(0x30, concat(derInteger((byte[]) n), derInteger((byte[]) e)));
                    byte[] bits = new byte[rsaKey.length + 1];  // no unused bits
                    System.arraycopy(rsaKey, 0, bits, 1, rsaKey.length);
                    spki = der(0x30, concat(RSA_ALGORITHM, der(0x03, bits)));
                }
            }
            return new CredentialKey((int) (long) (Long) algorithm, spki);
        }

        private static byte[] concat(byte[] a, byte[] b) {
            byte[] out = new byte[a.length + b.length];
            System.arraycopy(a, 0, out, 0, a.length);
            System.arraycopy(b, 0, out, a.length, b.length);
            return out;
        }

        private static byte[] derInteger(byte[] unsigned) {
            int start = 0;
            while (start < unsigned.length - 1 && unsigned[start] == 0) start++;
            boolean pad = unsigned.length == 0 || (unsigned[start] & 0x80) != 0;
            byte[] content = new byte[unsigned.length - start + (pad ? 1 : 0)];
            System.arraycopy(unsigned, start, content, pad ? 1 : 0, unsigned.length - start);
            return der(0x02, content);
        }

        private static byte[] der(int tag, byte[] content) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            out.write(tag);
            int length = content.length;
            if (length < 0x80) {
                out.write(length);
            } else if (length < 0x100) {
                out.write(0x81);
                out.write(length);
            } else {
                out.write(0x82);
                out.write(length >> 8);
                out.write(length);
            }
            out.write(content, 0, content.length);
            return out.toByteArray();
        }
    }

    /**
     * The definite-length CBOR a CTAP2 COSE_Key is made of (RFC 8949): integers (Long), byte
     * strings (byte[]), text strings, arrays and maps. Anything else is refused.
     */
    private static final class Cbor {
        private final byte[] mData;
        private int mAt;

        Cbor(byte[] data, int at) {
            mData = data;
            mAt = at;
        }

        private int unsignedByte() {
            if (mAt < 0 || mAt >= mData.length) throw new IllegalArgumentException("short CBOR");
            return mData[mAt++] & 0xff;
        }

        private long argument(int info) {
            if (info < 24) return info;
            if (info > 27) throw new IllegalArgumentException("indefinite or reserved CBOR");
            long value = 0;
            for (int i = 1 << (info - 24); i > 0; i--) value = value << 8 | unsignedByte();
            if (value < 0) throw new IllegalArgumentException("CBOR integer out of range");
            return value;
        }

        private int length(long argument) {
            if (argument > mData.length - mAt) throw new IllegalArgumentException("short CBOR");
            return (int) argument;
        }

        Object value(int depth) {
            if (depth > 4) throw new IllegalArgumentException("CBOR nested too deeply");
            int head = unsignedByte();
            long argument = argument(head & 0x1f);
            switch (head >> 5) {
                case 0:
                    return argument;
                case 1:
                    return -1 - argument;
                case 2: {
                    byte[] bytes = new byte[length(argument)];
                    System.arraycopy(mData, mAt, bytes, 0, bytes.length);
                    mAt += bytes.length;
                    return bytes;
                }
                case 3: {
                    int length = length(argument);
                    String text = new String(mData, mAt, length, StandardCharsets.UTF_8);
                    mAt += length;
                    return text;
                }
                case 4: {
                    List<Object> elements = new ArrayList<>();
                    for (int i = length(argument); i > 0; i--) elements.add(value(depth + 1));
                    return elements;
                }
                case 5: {
                    Map<Object, Object> members = new java.util.HashMap<>();
                    for (int i = length(argument); i > 0; i--) {
                        Object name = value(depth + 1);
                        members.put(name, value(depth + 1));
                    }
                    return members;
                }
                default:
                    throw new IllegalArgumentException("unsupported CBOR type");
            }
        }
    }
}
