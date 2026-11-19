/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (C) 2026 The Gesher Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package com.gesher.net;

import android.net.nsd.AdvertisingRequest;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Process;
import android.text.TextUtils;
import android.util.ArraySet;
import com.android.net.module.util.DnsUtils;
import com.gesher.net.mdns.MdnsAdvertisingOptions;
import java.nio.ByteBuffer;
import java.nio.CharBuffer;
import java.nio.charset.Charset;
import java.nio.charset.CharsetEncoder;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Android16r4 registration validation and the private Windows service boundary. */
public final class NsdRegistrationContract {
    private NsdRegistrationContract() {}
    private static final String TYPE_LABEL = "_[a-zA-Z0-9-_]{1,61}[a-zA-Z0-9]";
    private static final String SUBTYPE_LABEL = "_[\\x20-\\x2b\\x2d\\x2f-\\x7e]{1,62}";
    private static final Pattern TYPE = Pattern.compile("^(?:(" + SUBTYPE_LABEL + ")\\.)?("
            + TYPE_LABEL + "\\._(?:tcp|udp))\\.?((?:," + SUBTYPE_LABEL + ")*)$");
    private static final Pattern SUBTYPE = Pattern.compile("^" + SUBTYPE_LABEL + "$");
    private static final Pattern HOSTNAME = Pattern.compile(
            "^[a-zA-Z0-9]([a-zA-Z0-9-_]{0,61}[a-zA-Z0-9])?$");
    private static final int DNSSEC_PROTOCOL = 3;

    private enum ServiceValidationType { NO_SERVICE, HAS_SERVICE, HAS_SERVICE_ZERO_PORT }
    private enum HostValidationType { DEFAULT_HOST, CUSTOM_HOST, CUSTOM_HOST_NO_ADDRESS }

    private static ServiceValidationType validateService(NsdServiceInfo serviceInfo) {
        final boolean hasServiceName = !TextUtils.isEmpty(serviceInfo.getServiceName());
        final boolean hasServiceType = !TextUtils.isEmpty(serviceInfo.getServiceType());
        if (!hasServiceName && !hasServiceType && serviceInfo.getPort() == 0) {
            return ServiceValidationType.NO_SERVICE;
        }
        if (!hasServiceName || !hasServiceType) {
            throw new IllegalArgumentException("The service name or the service type is missing");
        }
        if (serviceInfo.getPort() < 0) throw new IllegalArgumentException("Invalid port");
        return serviceInfo.getPort() == 0 ? ServiceValidationType.HAS_SERVICE_ZERO_PORT
                : ServiceValidationType.HAS_SERVICE;
    }

    private static HostValidationType validateHost(NsdServiceInfo info) {
        if (TextUtils.isEmpty(info.getHostname())) return HostValidationType.DEFAULT_HOST;
        return info.getHostAddresses().isEmpty() ? HostValidationType.CUSTOM_HOST_NO_ADDRESS
                : HostValidationType.CUSTOM_HOST;
    }

    public static void checkServiceInfoForRegistration(NsdServiceInfo serviceInfo) {
        Objects.requireNonNull(serviceInfo, "NsdServiceInfo cannot be null");
        ServiceValidationType service = validateService(serviceInfo);
        HostValidationType host = validateHost(serviceInfo);
        byte[] key = serviceInfo.getPublicKey();
        if (key != null) {
            if (key.length < 4) {
                throw new IllegalArgumentException("The public key should be at least 4 bytes long");
            }
            if (key[2] != DNSSEC_PROTOCOL) {
                throw new IllegalArgumentException("The public key's protocol (" + key[2]
                        + ") is invalid. It should be DNSSEC_PROTOCOL (3)");
            }
        }
        if (service == ServiceValidationType.NO_SERVICE && host == HostValidationType.DEFAULT_HOST) {
            throw new IllegalArgumentException("Nothing to register");
        }
        if (key == null) {
            if (service == ServiceValidationType.HAS_SERVICE_ZERO_PORT) {
                throw new IllegalArgumentException("The port is missing");
            }
            if (service == ServiceValidationType.NO_SERVICE
                    && host == HostValidationType.CUSTOM_HOST_NO_ADDRESS) {
                throw new IllegalArgumentException(
                        "The host addresses must be specified unless there is a service");
            }
        }
    }

    public static void checkProtocol(int protocol) {
        if (protocol != NsdManager.PROTOCOL_DNS_SD) {
            throw new IllegalArgumentException("Unsupported protocol");
        }
    }

    /** The manager's exact implicit legacy comma-subtype update test. */
    public static boolean isSubtypeUpdate(NsdServiceInfo existing, NsdServiceInfo next) {
        if (existing == null || existing.getServiceType() == null || next.getServiceType() == null) {
            return false;
        }
        Matcher before = TYPE.matcher(existing.getServiceType());
        Matcher after = TYPE.matcher(next.getServiceType());
        return before.matches() && after.matches()
                && before.group(1) == null && after.group(1) == null
                && (!before.group(3).isEmpty() || !after.group(3).isEmpty())
                && Objects.equals(existing.getServiceName(), next.getServiceName())
                && Objects.equals(before.group(2), after.group(2));
    }

    /** A Binder-equivalent owned snapshot, also needed when Binder optimizes local calls. */
    public static NsdServiceInfo copyForTransport(NsdServiceInfo source) {
        NsdServiceInfo copy = new NsdServiceInfo(source);
        // The pinned value copy constructor is shallow for TXT and omits the key;
        // Parcel carries both. Preserve that distinction at this process boundary.
        for (Map.Entry<String, byte[]> entry : source.getAttributes().entrySet()) {
            copy.setAttribute(entry.getKey(), entry.getValue() == null
                    ? (byte[]) null : entry.getValue().clone());
        }
        copy.setPublicKey(source.getPublicKey());
        return copy;
    }

    /** Service-side failures are callbacks, distinct from manager argument exceptions. */
    public static final class Failure extends Exception {
        public final int error;
        public Failure(int error) { this.error = error; }
    }

    public static final class Prepared {
        public final NsdServiceInfo service;
        public final MdnsAdvertisingOptions options;
        Prepared(NsdServiceInfo service, MdnsAdvertisingOptions options) {
            this.service = service; this.options = options;
        }
    }

    /** Called after quota/ownership admission, using the captured Binder UID. */
    public static Prepared normalize(AdvertisingRequest request, int uid) throws Failure {
        NsdServiceInfo info = copyForTransport(request.getServiceInfo());
        String type = info.getServiceType();
        Matcher parsed = type == null ? null : TYPE.matcher(type);
        if (type != null && !parsed.matches()) {
            throw new Failure(NsdManager.FAILURE_INTERNAL_ERROR);
        }
        String hostname = info.getHostname();
        if (hostname == null) info.setHostAddresses(Collections.emptyList());
        if (parsed != null) {
            info.setServiceType(parsed.group(2));
            info.setServiceName(truncateServiceName(info.getServiceName(), 63));
        }
        if (hostname != null && !HOSTNAME.matcher(hostname).matches()) {
            throw new Failure(NsdManager.FAILURE_BAD_PARAMETERS);
        }
        byte[] key = info.getPublicKey();
        if (key != null && (key.length < 4 || key[2] != DNSSEC_PROTOCOL)) {
            throw new Failure(NsdManager.FAILURE_BAD_PARAMETERS);
        }
        Set<String> subtypes = new ArraySet<>(info.getSubtypes());
        if (parsed != null) {
            if (parsed.group(1) != null) subtypes.add(parsed.group(1));
            else if (!parsed.group(3).isEmpty()) {
                Collections.addAll(subtypes, parsed.group(3).substring(1).split(","));
            }
        }
        Map<String, String> distinct = new LinkedHashMap<>();
        for (String subtype : subtypes) distinct.put(DnsUtils.toDnsUpperCase(subtype), subtype);
        subtypes = new ArraySet<>(distinct.values());
        if (subtypes.size() > 100) throw new Failure(NsdManager.FAILURE_BAD_PARAMETERS);
        for (String subtype : subtypes) {
            if (!SUBTYPE.matcher(subtype).matches()) {
                throw new Failure(NsdManager.FAILURE_BAD_PARAMETERS);
            }
        }
        Duration ttl = request.getTtl();
        if (ttl != null) {
            long seconds = ttl.toSeconds();
            long minimum = uid == Process.SYSTEM_UID ? 0 : 30;
            long maximum = uid == Process.SYSTEM_UID ? 0xffffffffL : 36000;
            if (seconds < minimum || seconds > maximum) {
                throw new Failure(NsdManager.FAILURE_BAD_PARAMETERS);
            }
        }
        long flags = request.getFlags();
        if ((flags & AdvertisingRequest.FLAG_OFFLOAD_ONLY) != 0) {
            // This backend has no hardware offload provider. Do not silently turn
            // an offload-only request into an ordinary network advertisement.
            throw new Failure(NsdManager.FAILURE_BAD_PARAMETERS);
        }
        info.setSubtypes(subtypes);
        return new Prepared(info, MdnsAdvertisingOptions.newBuilder()
                .setIsOnlyUpdate((flags & AdvertisingRequest.NSD_ADVERTISING_UPDATE_ONLY) != 0)
                .setSkipProbing((flags & AdvertisingRequest.FLAG_SKIP_PROBING) != 0)
                .setSkipSubtypeAnnouncements(
                        (flags & AdvertisingRequest.FLAG_SKIP_SUBTYPE_ANNOUNCEMENTS) != 0)
                .setTtl(ttl).build());
    }

    /** Pinned MdnsUtils helper, preserving its UTF-8 encoder boundary behavior. */
    public static String truncateServiceName(String originalName, int maxLength) {
        if (originalName.length() <= maxLength / 4) return originalName;
        final Charset utf8 = StandardCharsets.UTF_8;
        final CharsetEncoder encoder = utf8.newEncoder();
        final ByteBuffer out = ByteBuffer.allocate(maxLength);
        encoder.encode(CharBuffer.wrap(originalName), out, true);
        return new String(out.array(), 0, out.position(), utf8);
    }
}
