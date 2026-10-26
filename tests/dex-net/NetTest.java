/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.math.BigInteger;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.HttpURLConnection;
import java.net.Inet4Address;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.net.UnknownHostException;
import java.nio.ByteBuffer;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;
import java.nio.charset.StandardCharsets;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.MessageDigest;
import java.security.Provider;
import java.security.SecureRandom;
import java.security.Security;
import java.security.Signature;
import java.security.cert.CertificateException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Iterator;
import java.util.List;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.Mac;
import javax.crypto.SecretKey;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.PBEKeySpec;
import javax.crypto.spec.SecretKeySpec;
import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLException;
import javax.net.ssl.SSLHandshakeException;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLPeerUnverifiedException;
import javax.net.ssl.SSLSession;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;

/**
 * Networking from Java on Gesher (docs/network.md): libcore's sockets and DNS
 * over gesher-base, NativeBN and Conscrypt over BoringSSL, OkHttp behind
 * HttpURLConnection, and the system CA certificates from Windows' store.
 *
 * <pre>
 *   NetTest local                 sockets, DNS, NIO, HTTP and crypto on this machine
 *   NetTest echo PORT             a TCP and a UDP echo server on 127.0.0.1:PORT (not Java)
 *   NetTest internet              DNS, HTTPS (TLS 1.3, SNI) and bad certificates rejected
 *   NetTest https URL ok|reject   one HTTPS GET, expected to pass or to fail on the certificate
 * </pre>
 * Prints "OK <check>" per check and "NetTest all passed" at the end.
 */
public final class NetTest {
    private static int failures;

    private interface Check {
        String run() throws Exception;
    }

    private static void check(String name, Check check) {
        try {
            String detail = check.run();
            System.out.println("OK    " + name + (detail == null ? "" : ": " + detail));
        } catch (Throwable t) {
            failures++;
            System.out.println("FAIL  " + name + ": " + t);
            t.printStackTrace(System.out);
        }
    }

    private static void require(boolean condition, String what) {
        if (!condition) throw new AssertionError(what);
    }

    public static void main(String[] args) throws Exception {
        String mode = args.length > 0 ? args[0] : "local";
        switch (mode) {
            case "local":
                local();
                break;
            case "echo":
                echo(Integer.parseInt(args[1]));
                break;
            case "internet":
                internet();
                break;
            case "https":
                https(args[1], args[2].equals("ok"));
                break;
            default:
                throw new IllegalArgumentException(mode);
        }
        System.out.println(failures == 0 ? "NetTest all passed" : "NetTest: " + failures + " failed");
        System.exit(failures == 0 ? 0 : 1);
    }

    // --- on this machine ------------------------------------------------------

    private static void local() throws Exception {
        check("DNS: localhost and literals", () -> {
            InetAddress[] all = InetAddress.getAllByName("localhost");
            require(all.length > 0 && all[0].isLoopbackAddress(), "localhost: " + Arrays.toString(all));
            InetAddress v4 = InetAddress.getByName("127.0.0.1");
            require(v4 instanceof Inet4Address && v4.isLoopbackAddress(), "127.0.0.1");
            InetAddress v6 = InetAddress.getByName("::1");
            require(v6 instanceof Inet6Address && v6.isLoopbackAddress(), "::1");
            String host = InetAddress.getLocalHost().getHostName();
            return "localhost=" + Arrays.toString(all) + ", this host " + host;
        });
        check("DNS: an unknown name fails with UnknownHostException", () -> {
            try {
                InetAddress.getByName("no-such-host.invalid");
            } catch (UnknownHostException expected) {
                return expected.getMessage();
            }
            throw new AssertionError("resolved no-such-host.invalid");
        });
        check("TCP: ServerSocket and Socket on loopback, both directions", () -> {
            try (ServerSocket server = new ServerSocket(0, 50, InetAddress.getByName("127.0.0.1"))) {
                Thread t = new Thread(() -> {
                    try (Socket s = server.accept()) {
                        echoStream(s.getInputStream(), s.getOutputStream());
                    } catch (IOException e) {
                        throw new RuntimeException(e);
                    }
                });
                t.start();
                String reply = tcpRoundTrip("127.0.0.1", server.getLocalPort(), 100_000);
                t.join(10_000);
                return reply;
            }
        });
        check("TCP: IPv6 loopback, a read timeout", () -> {
            try (ServerSocket server = new ServerSocket()) {
                server.bind(new InetSocketAddress(InetAddress.getByName("::1"), 0));
                try (Socket client = new Socket()) {
                    client.connect(server.getLocalSocketAddress(), 5000);
                    try (Socket accepted = server.accept()) {
                        client.setSoTimeout(300);
                        long start = System.nanoTime();
                        try {
                            client.getInputStream().read();
                            throw new AssertionError("read returned with nothing sent");
                        } catch (SocketTimeoutException expected) {
                        }
                        long ms = (System.nanoTime() - start) / 1_000_000;
                        require(ms >= 250, "timed out after " + ms + " ms");
                        accepted.getOutputStream().write(42);
                        require(client.getInputStream().read() == 42, "byte after the timeout");
                        return "peer " + accepted.getRemoteSocketAddress() + ", timeout after " + ms + " ms";
                    }
                }
            }
        });
        check("TCP: connecting to a closed port is refused", () -> {
            int port;
            try (ServerSocket s = new ServerSocket(0)) {
                port = s.getLocalPort();
            }
            try (Socket s = new Socket()) {
                s.connect(new InetSocketAddress("127.0.0.1", port), 5000);
                throw new AssertionError("connected to a closed port");
            } catch (java.net.ConnectException expected) {
                return expected.getMessage();
            }
        });
        check("UDP: DatagramSocket round trip on loopback", () -> {
            try (DatagramSocket a = new DatagramSocket(0, InetAddress.getByName("127.0.0.1"));
                 DatagramSocket b = new DatagramSocket(0, InetAddress.getByName("127.0.0.1"))) {
                b.setSoTimeout(5000);
                byte[] data = "datagram".getBytes(StandardCharsets.UTF_8);
                a.send(new DatagramPacket(data, data.length, b.getLocalSocketAddress()));
                DatagramPacket p = new DatagramPacket(new byte[100], 100);
                b.receive(p);
                String got = new String(p.getData(), 0, p.getLength(), StandardCharsets.UTF_8);
                require(got.equals("datagram") && p.getPort() == a.getLocalPort(), got);
                return got + " from " + p.getSocketAddress();
            }
        });
        check("NIO: non-blocking channels with a Selector", NetTest::nioEcho);
        check("HTTP: HttpURLConnection (OkHttp) GET and POST to a local server", NetTest::localHttp);
        check("BigInteger: NativeBN over BoringSSL", () -> {
            BigInteger p = new BigInteger("170141183460469231731687303715884105727");  // 2^127 - 1
            require(p.isProbablePrime(50), "2^127-1 prime");
            BigInteger a = new BigInteger(1, new byte[64]).add(BigInteger.valueOf(123456789));
            BigInteger r = a.pow(7).modPow(BigInteger.valueOf(65537), p);
            BigInteger check = a.modPow(BigInteger.valueOf(7L * 65537), p);
            require(r.equals(check), "modPow");
            BigInteger big = BigInteger.ONE.shiftLeft(4000).subtract(BigInteger.ONE);
            require(big.multiply(big).bitLength() == 8000, "multiply");
            require(big.gcd(big.subtract(BigInteger.ONE)).equals(BigInteger.ONE), "gcd");
            return "modPow, multiply, gcd, isProbablePrime";
        });
        check("JCA: Android's providers (Conscrypt first, BouncyCastle)", () -> {
            List<String> names = new ArrayList<>();
            for (Provider p : Security.getProviders()) names.add(p.getName());
            require(!names.isEmpty() && names.get(0).equals("AndroidOpenSSL"), "providers " + names);
            require(names.contains("BC"), "providers " + names);
            return names.toString();
        });
        check("JCA: digests, HMAC, AES-GCM, RSA and EC signatures, SecureRandom, PBKDF2", () -> {
            MessageDigest sha = MessageDigest.getInstance("SHA-256");
            String hex = new BigInteger(1, sha.digest("abc".getBytes(StandardCharsets.US_ASCII))).toString(16);
            require(hex.equals("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha-256 " + hex);
            require(sha.getProvider().getName().equals("AndroidOpenSSL"), "sha-256 from " + sha.getProvider());
            Mac mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec("key".getBytes(StandardCharsets.US_ASCII), "HmacSHA256"));
            require(mac.doFinal("msg".getBytes(StandardCharsets.US_ASCII)).length == 32, "hmac");
            SecretKey key = KeyGenerator.getInstance("AES").generateKey();
            byte[] iv = new byte[12];
            SecureRandom random = new SecureRandom();
            random.nextBytes(iv);
            Cipher c = Cipher.getInstance("AES/GCM/NoPadding");
            c.init(Cipher.ENCRYPT_MODE, key, new GCMParameterSpec(128, iv));
            byte[] sealed = c.doFinal("secret".getBytes(StandardCharsets.UTF_8));
            c.init(Cipher.DECRYPT_MODE, key, new GCMParameterSpec(128, iv));
            require(new String(c.doFinal(sealed), StandardCharsets.UTF_8).equals("secret"), "aes-gcm");
            for (String[] alg : new String[][] {{"RSA", "SHA256withRSA"}, {"EC", "SHA256withECDSA"}}) {
                KeyPairGenerator g = KeyPairGenerator.getInstance(alg[0]);
                g.initialize(alg[0].equals("RSA") ? 2048 : 256);
                KeyPair kp = g.generateKeyPair();
                Signature s = Signature.getInstance(alg[1]);
                s.initSign(kp.getPrivate());
                s.update(sealed);
                byte[] sig = s.sign();
                s.initVerify(kp.getPublic());
                s.update(sealed);
                require(s.verify(sig), alg[1]);
            }
            SecretKeyFactory f = SecretKeyFactory.getInstance("PBKDF2WithHmacSHA1");
            byte[] derived = f.generateSecret(new PBEKeySpec("pw".toCharArray(), new byte[8], 1000, 128)).getEncoded();
            require(derived.length == 16, "pbkdf2");
            return "SecureRandom " + random.getAlgorithm() + " (" + random.getProvider().getName()
                    + "), PBKDF2 from " + f.getProvider().getName();
        });
        check("TLS: SSLContext default (Conscrypt), protocols", () -> {
            SSLContext context = SSLContext.getDefault();
            String[] protocols = context.getDefaultSSLParameters().getProtocols();
            require(Arrays.asList(protocols).contains("TLSv1.3"), Arrays.toString(protocols));
            return context.getProvider().getName() + " " + Arrays.toString(protocols);
        });
    }

    private static void echoStream(InputStream in, OutputStream out) throws IOException {
        byte[] buffer = new byte[8192];
        for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
        out.flush();
    }

    /** Sends `size` bytes to an echo server, reads them back, and compares. */
    private static String tcpRoundTrip(String host, int port, int size) throws Exception {
        byte[] data = new byte[size];
        new SecureRandom().nextBytes(data);
        try (Socket s = new Socket()) {
            s.connect(new InetSocketAddress(host, port), 5000);
            s.setSoTimeout(15_000);
            Thread writer = new Thread(() -> {
                try {
                    s.getOutputStream().write(data);
                    s.shutdownOutput();
                } catch (IOException e) {
                    throw new RuntimeException(e);
                }
            });
            writer.start();
            ByteArrayOutputStream got = new ByteArrayOutputStream();
            echoStream(s.getInputStream(), got);
            writer.join(10_000);
            require(Arrays.equals(got.toByteArray(), data), "echoed " + got.size() + " of " + size + " bytes");
            return size + " bytes echoed by " + s.getRemoteSocketAddress();
        }
    }

    private static String nioEcho() throws Exception {
        try (Selector selector = Selector.open();
             ServerSocketChannel server = ServerSocketChannel.open()) {
            server.bind(new InetSocketAddress("127.0.0.1", 0));
            server.configureBlocking(false);
            server.register(selector, SelectionKey.OP_ACCEPT);
            SocketChannel client = SocketChannel.open();
            client.configureBlocking(false);
            client.connect(server.getLocalAddress());
            client.register(selector, SelectionKey.OP_CONNECT);
            ByteBuffer out = ByteBuffer.wrap("over NIO".getBytes(StandardCharsets.UTF_8));
            ByteBuffer back = ByteBuffer.allocate(64);
            long deadline = System.currentTimeMillis() + 10_000;
            int events = 0;
            while (back.position() < out.capacity()) {
                require(System.currentTimeMillis() < deadline, "timed out after " + events + " events");
                if (selector.select(1000) == 0) continue;
                Iterator<SelectionKey> keys = selector.selectedKeys().iterator();
                while (keys.hasNext()) {
                    SelectionKey key = keys.next();
                    keys.remove();
                    events++;
                    if (key.isAcceptable()) {
                        SocketChannel accepted = server.accept();
                        accepted.configureBlocking(false);
                        accepted.register(selector, SelectionKey.OP_READ, ByteBuffer.allocate(64));
                    } else if (key.isConnectable()) {
                        require(client.finishConnect(), "finishConnect");
                        key.interestOps(SelectionKey.OP_WRITE);
                    } else if (key.channel() == client && key.isWritable()) {
                        client.write(out);
                        if (!out.hasRemaining()) key.interestOps(SelectionKey.OP_READ);
                    } else if (key.channel() == client && key.isReadable()) {
                        client.read(back);
                    } else if (key.isReadable()) {
                        // The server side echoes what it reads.
                        SocketChannel ch = (SocketChannel) key.channel();
                        ByteBuffer buf = (ByteBuffer) key.attachment();
                        buf.clear();
                        if (ch.read(buf) < 0) {
                            ch.close();
                            continue;
                        }
                        buf.flip();
                        while (buf.hasRemaining()) ch.write(buf);
                    }
                }
            }
            client.close();
            String got = new String(back.array(), 0, back.position(), StandardCharsets.UTF_8);
            require(got.equals("over NIO"), got);
            return "'" + got + "' after " + events + " selected keys";
        }
    }

    private static String localHttp() throws Exception {
        try (ServerSocket server = new ServerSocket(0, 50, InetAddress.getByName("127.0.0.1"))) {
            Thread t = new Thread(() -> {
                for (int i = 0; i < 2; i++) {
                    try (Socket s = server.accept()) {
                        InputStream in = s.getInputStream();
                        StringBuilder head = new StringBuilder();
                        while (!head.toString().endsWith("\r\n\r\n")) {
                            int c = in.read();
                            if (c < 0) break;
                            head.append((char) c);
                        }
                        String request = head.toString();
                        int length = 0;
                        for (String line : request.split("\r\n")) {
                            if (line.toLowerCase().startsWith("content-length:")) {
                                length = Integer.parseInt(line.substring(15).trim());
                            }
                        }
                        byte[] body = new byte[length];
                        for (int n = 0; n < length; ) n += in.read(body, n, length - n);
                        String reply = request.split(" ")[0] + " " + request.split(" ")[1] + " "
                                + new String(body, StandardCharsets.UTF_8);
                        byte[] bytes = reply.getBytes(StandardCharsets.UTF_8);
                        OutputStream out = s.getOutputStream();
                        out.write(("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: "
                                + bytes.length + "\r\nConnection: close\r\n\r\n")
                                .getBytes(StandardCharsets.US_ASCII));
                        out.write(bytes);
                        out.flush();
                    } catch (IOException e) {
                        throw new RuntimeException(e);
                    }
                }
            });
            t.start();
            URL url = new URL("http://127.0.0.1:" + server.getLocalPort() + "/hello?x=1");
            HttpURLConnection get = (HttpURLConnection) url.openConnection();
            require(get.getClass().getName().startsWith("com.android.okhttp"), get.getClass().getName());
            String a = read(get);
            require(get.getResponseCode() == 200 && a.equals("GET /hello?x=1 "), a);
            HttpURLConnection post = (HttpURLConnection) url.openConnection();
            post.setDoOutput(true);
            post.getOutputStream().write("body".getBytes(StandardCharsets.UTF_8));
            String b = read(post);
            require(b.equals("POST /hello?x=1 body"), b);
            t.join(10_000);
            return get.getClass().getName() + ": '" + a + "', '" + b + "'";
        }
    }

    private static String read(HttpURLConnection c) throws IOException {
        c.setConnectTimeout(15_000);
        c.setReadTimeout(30_000);
        try (InputStream in = c.getInputStream()) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            in.transferTo(out);
            return out.toString(StandardCharsets.UTF_8);
        }
    }

    // --- an echo server that is not Java ------------------------------------------

    private static void echo(int port) throws Exception {
        check("TCP: echo by a Windows server on 127.0.0.1:" + port,
                () -> tcpRoundTrip("127.0.0.1", port, 50_000));
        check("UDP: echo by a Windows server on 127.0.0.1:" + port, () -> {
            try (DatagramSocket s = new DatagramSocket()) {
                s.setSoTimeout(5000);
                byte[] data = "to windows and back".getBytes(StandardCharsets.UTF_8);
                s.send(new DatagramPacket(data, data.length, InetAddress.getByName("127.0.0.1"), port));
                DatagramPacket p = new DatagramPacket(new byte[100], 100);
                s.receive(p);
                String got = new String(p.getData(), 0, p.getLength(), StandardCharsets.UTF_8);
                require(got.equals("to windows and back"), got);
                return got;
            }
        });
    }

    // --- on the internet ----------------------------------------------------------

    private static void internet() throws Exception {
        check("DNS: example.com and www.google.com", () -> {
            InetAddress[] a = InetAddress.getAllByName("example.com");
            InetAddress[] b = InetAddress.getAllByName("www.google.com");
            require(a.length > 0 && b.length > 0, "no addresses");
            return "example.com " + Arrays.toString(a) + ", www.google.com " + b.length + " addresses";
        });
        check("HTTPS: GET https://example.com/ (HttpsURLConnection, OkHttp over Conscrypt)", () -> {
            HttpsURLConnection c = (HttpsURLConnection) new URL("https://example.com/").openConnection();
            String body = read(c);
            require(c.getResponseCode() == 200 && body.contains("Example Domain"), "status " + c.getResponseCode());
            return c.getCipherSuite() + ", " + body.length() + " characters";
        });
        check("HTTPS: GET https://www.google.com/generate_204", () -> {
            HttpsURLConnection c = (HttpsURLConnection) new URL("https://www.google.com/generate_204").openConnection();
            read(c);
            require(c.getResponseCode() == 204, "status " + c.getResponseCode());
            return "204, " + c.getCipherSuite();
        });
        check("TLS 1.3 with SNI and hostname verification (SSLSocket to www.google.com)", () -> {
            SSLSocketFactory f = (SSLSocketFactory) SSLSocketFactory.getDefault();
            try (SSLSocket s = (SSLSocket) f.createSocket("www.google.com", 443)) {
                SSLParameters p = s.getSSLParameters();
                p.setEndpointIdentificationAlgorithm("HTTPS");
                s.setSSLParameters(p);
                s.setSoTimeout(15_000);
                s.startHandshake();
                SSLSession session = s.getSession();
                require(session.getProtocol().equals("TLSv1.3"), session.getProtocol());
                return session.getProtocol() + " " + session.getCipherSuite() + ", peer "
                        + session.getPeerPrincipal();
            }
        });
        check("TLS: SNI picks the virtual host (https://badssl.com/ among badssl.com's hosts)", () -> {
            HttpsURLConnection c = (HttpsURLConnection) new URL("https://badssl.com/").openConnection();
            String body = read(c);
            require(c.getResponseCode() == 200 && body.contains("badssl"), "status " + c.getResponseCode());
            return "badssl.com served its own certificate";
        });
        String[][] bad = {
            {"https://expired.badssl.com/", "an expired certificate"},
            {"https://wrong.host.badssl.com/", "a certificate for another host"},
            {"https://self-signed.badssl.com/", "a self-signed certificate"},
            {"https://untrusted-root.badssl.com/", "an untrusted root"},
        };
        String local = System.getenv("GESHER_TLS_REJECTION_URLS");
        if (local != null) {
            String[] urls = local.split(",", -1);
            require(urls.length == bad.length, "four certificate test URLs required");
            for (int i = 0; i < bad.length; i++) bad[i][0] = urls[i];
        }
        for (String[] b : bad) {
            check("HTTPS: " + b[0] + " is rejected (" + b[1] + ")", () -> rejected(b[0]));
        }
    }

    private static String rejected(String url) throws Exception {
        HttpsURLConnection c = (HttpsURLConnection) new URL(url).openConnection();
        try {
            read(c);
        } catch (SSLPeerUnverifiedException e) {
            return e.getClass().getSimpleName() + ": " + rootCause(e);
        } catch (SSLException e) {
            for (Throwable cause = e; cause != null; cause = cause.getCause()) {
                if (cause instanceof CertificateException) {
                    return e.getClass().getSimpleName() + ": " + rootCause(e);
                }
            }
            throw e;
        }
        throw new AssertionError("accepted " + url + " (" + c.getResponseCode() + ")");
    }

    private static Throwable rootCause(Throwable t) {
        while (t.getCause() != null && t.getCause() != t) t = t.getCause();
        return t;
    }

    private static void https(String url, boolean ok) throws Exception {
        check("HTTPS: " + url + (ok ? " is trusted" : " is rejected"), () -> {
            if (!ok) return rejected(url);
            HttpsURLConnection c = (HttpsURLConnection) new URL(url).openConnection();
            String body = read(c);
            require(c.getResponseCode() == 200, "status " + c.getResponseCode());
            return "'" + body.trim() + "', " + c.getCipherSuite() + ", issuer "
                    + ((java.security.cert.X509Certificate) c.getServerCertificates()[0]).getIssuerX500Principal();
        });
    }
}
