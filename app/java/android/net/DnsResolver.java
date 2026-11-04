/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net;

import android.os.CancellationSignal;

import com.gesher.net.WindowsNetworks;

import java.io.ByteArrayOutputStream;
import java.net.Inet4Address;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.UnknownHostException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executor;

/**
 * The Connectivity module's DnsResolver (framework.jar compiles against the module's API
 * and does not contain it), with its public API. Apps that name the class (Waze's network
 * stack, when an address is chosen) died with NoClassDefFoundError.
 *
 * <p>A lookup runs on a thread of its own and resolves A and AAAA records through the
 * network's Windows interface, as {@link Network#getAllByName} does (the process's
 * network, or Windows' default, for a null network); its callback runs on the executor
 * unless the lookup was cancelled. A name that does not resolve is answered with no
 * addresses and NXDOMAIN, as the module does. rawQuery answers A and AAAA questions of
 * class IN with a DNS response built from those addresses (TTL 0); Windows' resolver
 * gives no records of other types, so they are answered NOTIMP.
 */
public final class DnsResolver {
    public static final int CLASS_IN = 1;
    public static final int TYPE_A = 1;
    public static final int TYPE_AAAA = 28;

    public static final int FLAG_EMPTY = 0;
    public static final int FLAG_NO_RETRY = 1 << 0;
    public static final int FLAG_NO_CACHE_STORE = 1 << 1;
    public static final int FLAG_NO_CACHE_LOOKUP = 1 << 2;

    public static final int ERROR_PARSE = 0;
    public static final int ERROR_SYSTEM = 1;

    private static final int RCODE_NOERROR = 0;
    private static final int RCODE_FORMERR = 1;
    private static final int RCODE_NXDOMAIN = 3;
    private static final int RCODE_NOTIMP = 4;

    private static final DnsResolver sInstance = new DnsResolver();

    public static DnsResolver getInstance() {
        return sInstance;
    }

    private DnsResolver() {
    }

    public interface Callback<T> {
        void onAnswer(T answer, int rcode);

        void onError(DnsException error);
    }

    public static class DnsException extends Exception {
        public final int code;

        public DnsException(int code, Throwable cause) {
            super(cause);
            this.code = code;
        }
    }

    public void rawQuery(Network network, byte[] query, int flags, Executor executor,
            CancellationSignal cancellationSignal, Callback<? super byte[]> callback) {
        check(executor, callback);
        if (query == null) throw new NullPointerException("query");
        byte[] copy = query.clone();
        run(cancellationSignal, executor, callback, () -> answerRaw(network, copy));
    }

    public void rawQuery(Network network, String domain, int nsClass, int nsType, int flags,
            Executor executor, CancellationSignal cancellationSignal,
            Callback<? super byte[]> callback) {
        check(executor, callback);
        if (domain == null) throw new NullPointerException("domain");
        run(cancellationSignal, executor, callback, () -> {
            byte[] query;
            try {
                query = question(domain, nsClass, nsType);
            } catch (IllegalArgumentException e) {
                throw new DnsException(ERROR_PARSE, e);
            }
            return answerRaw(network, query);
        });
    }

    public void query(Network network, String domain, int flags, Executor executor,
            CancellationSignal cancellationSignal, Callback<? super List<InetAddress>> callback) {
        query(network, domain, -1, flags, executor, cancellationSignal, callback);
    }

    public void query(Network network, String domain, int nsType, int flags, Executor executor,
            CancellationSignal cancellationSignal, Callback<? super List<InetAddress>> callback) {
        check(executor, callback);
        if (domain == null) throw new NullPointerException("domain");
        run(cancellationSignal, executor, callback, () -> {
            List<InetAddress> addresses = new ArrayList<>();
            int rcode = lookup(network, domain, nsType, addresses);
            return new Answer<>(addresses, rcode);
        });
    }

    private static void check(Executor executor, Callback<?> callback) {
        if (executor == null) throw new NullPointerException("executor");
        if (callback == null) throw new NullPointerException("callback");
    }

    private interface Lookup<T> {
        Answer<T> call() throws DnsException;
    }

    private static final class Answer<T> {
        final T value;
        final int rcode;

        Answer(T value, int rcode) {
            this.value = value;
            this.rcode = rcode;
        }
    }

    private static <T> void run(CancellationSignal signal, Executor executor,
            Callback<? super T> callback, Lookup<T> lookup) {
        if (signal != null && signal.isCanceled()) return;
        Thread thread = new Thread(() -> {
            Answer<T> answer = null;
            DnsException error = null;
            try {
                answer = lookup.call();
            } catch (DnsException e) {
                error = e;
            } catch (RuntimeException e) {
                error = new DnsException(ERROR_SYSTEM, e);
            }
            if (signal != null && signal.isCanceled()) return;
            Answer<T> a = answer;
            DnsException e = error;
            executor.execute(() -> {
                if (signal != null && signal.isCanceled()) return;
                if (e != null) {
                    callback.onError(e);
                } else {
                    callback.onAnswer(a.value, a.rcode);
                }
            });
        }, "DnsResolver");
        thread.setDaemon(true);
        thread.start();
    }

    /** Resolves {@code domain}'s addresses of {@code nsType} (-1: both) into {@code out}. */
    private static int lookup(Network network, String domain, int nsType, List<InetAddress> out)
            throws DnsException {
        int netId = network != null ? network.netId : WindowsNetworks.processNetwork();
        InetAddress[] all;
        try {
            all = WindowsNetworks.resolve(netId, domain);
        } catch (UnknownHostException e) {
            return RCODE_NXDOMAIN;
        } catch (IllegalArgumentException e) {
            throw new DnsException(ERROR_SYSTEM, e);
        }
        for (InetAddress address : all) {
            if (nsType == -1 || (nsType == TYPE_A && address instanceof Inet4Address)
                    || (nsType == TYPE_AAAA && address instanceof Inet6Address)) {
                out.add(address);
            }
        }
        return RCODE_NOERROR;
    }

    private static Answer<byte[]> answerRaw(Network network, byte[] query) throws DnsException {
        // Header (12 bytes), then one question: name, type, class.
        if (query.length < 12) throw new DnsException(ERROR_PARSE, null);
        int qdcount = u16(query, 4);
        int pos = 12;
        StringBuilder name = new StringBuilder();
        try {
            while (true) {
                int len = query[pos++] & 0xff;
                if (len == 0) break;
                if (len > 63) throw new DnsException(ERROR_PARSE, null);
                if (name.length() > 0) name.append('.');
                name.append(new String(query, pos, len, StandardCharsets.ISO_8859_1));
                pos += len;
            }
            if (pos + 4 > query.length) throw new DnsException(ERROR_PARSE, null);
        } catch (ArrayIndexOutOfBoundsException | StringIndexOutOfBoundsException e) {
            throw new DnsException(ERROR_PARSE, e);
        }
        int qtype = u16(query, pos);
        int qclass = u16(query, pos + 2);
        int questionEnd = pos + 4;

        List<InetAddress> addresses = new ArrayList<>();
        int rcode;
        if (qdcount != 1) {
            rcode = RCODE_FORMERR;
        } else if (qclass != CLASS_IN || (qtype != TYPE_A && qtype != TYPE_AAAA)) {
            rcode = RCODE_NOTIMP;
        } else {
            rcode = lookup(network, name.toString(), qtype, addresses);
        }

        ByteArrayOutputStream out = new ByteArrayOutputStream();
        out.write(query[0]);  // id
        out.write(query[1]);
        // QR, the query's opcode and RD, RA; then the rcode.
        out.write(0x80 | (query[2] & 0x79));
        out.write(0x80 | rcode);
        put16(out, qdcount == 1 ? 1 : 0);
        put16(out, addresses.size());
        put16(out, 0);
        put16(out, 0);
        if (qdcount == 1) out.write(query, 12, questionEnd - 12);
        for (InetAddress address : addresses) {
            byte[] raw = address.getAddress();
            put16(out, 0xc00c);  // a pointer to the question's name
            put16(out, raw.length == 4 ? TYPE_A : TYPE_AAAA);
            put16(out, CLASS_IN);
            put16(out, 0);  // TTL 0: Windows' resolver does not give it
            put16(out, 0);
            put16(out, raw.length);
            out.write(raw, 0, raw.length);
        }
        return new Answer<>(out.toByteArray(), rcode);
    }

    /** A recursive query for one question, with a random id. */
    private static byte[] question(String domain, int nsClass, int nsType) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        put16(out, (int) (Math.random() * 0x10000));
        put16(out, 0x0100);  // RD
        put16(out, 1);
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        for (String label : domain.split("\\.")) {
            if (label.isEmpty()) continue;
            byte[] bytes = label.getBytes(StandardCharsets.ISO_8859_1);
            if (bytes.length > 63) throw new IllegalArgumentException("label too long: " + label);
            out.write(bytes.length);
            out.write(bytes, 0, bytes.length);
        }
        out.write(0);
        put16(out, nsType);
        put16(out, nsClass);
        return out.toByteArray();
    }

    private static int u16(byte[] b, int at) {
        return ((b[at] & 0xff) << 8) | (b[at + 1] & 0xff);
    }

    private static void put16(ByteArrayOutputStream out, int v) {
        out.write((v >> 8) & 0xff);
        out.write(v & 0xff);
    }
}
