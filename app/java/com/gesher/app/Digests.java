package com.gesher.app;

import java.security.MessageDigestSpi;
import java.security.Provider;
import java.security.Security;
import java.util.ArrayList;
import java.util.List;

/**
 * The JCA providers gesher-app runs with.
 *
 * <p>libcore's security.properties names Conscrypt's and BouncyCastle's
 * providers, and sun.security.jca.Providers asserts that every configured
 * provider can be instantiated. Neither is on Gesher's boot class path yet, so
 * the first JCA use (the framework digests partition fingerprints with SHA-1
 * in SystemProperties.digestOf) fails. {@link #install} keeps the configured
 * providers whose classes exist and puts this provider first, with SHA-1,
 * SHA-256 and MD5 MessageDigests. It must run before anything uses JCA. Once
 * the boot class path carries BouncyCastle or Conscrypt this provider only
 * comes first; their algorithms are still found after it.
 */
public final class Digests extends Provider {
    public Digests() {
        super("GesherDigests", 1.0,
                "Gesher's MessageDigests (SHA-1, SHA-256, MD5), X.509 certificates and SecureRandom");
        put("MessageDigest.SHA-1", Sha1.class.getName());
        put("Alg.Alias.MessageDigest.SHA1", "SHA-1");
        put("Alg.Alias.MessageDigest.SHA", "SHA-1");
        put("MessageDigest.SHA-256", Sha256.class.getName());
        put("Alg.Alias.MessageDigest.SHA256", "SHA-256");
        put("MessageDigest.MD5", Md5.class.getName());
        // X.509 certificates over libcore's own parser (Conscrypt's factory is not there
        // yet): the network security config reads the system CA store with one.
        put("CertificateFactory.X.509", X509.class.getName());
        put("Alg.Alias.CertificateFactory.X509", "X.509");
        // SecureRandom from the system's generator (/dev/urandom is BCryptGenRandom in
        // gesher-base): new SecureRandom() and UUID.randomUUID() need one (docs/e2e.md:
        // AndroidX Fragment's constructor), Conscrypt's OpenSSLRandom is not there yet.
        put("SecureRandom.NativePRNG", Urandom.class.getName());
        put("Alg.Alias.SecureRandom.SHA1PRNG", "NativePRNG");
    }

    /** SecureRandom over /dev/urandom; seeding adds nothing to the system's generator. */
    public static final class Urandom extends java.security.SecureRandomSpi {
        private static java.io.FileInputStream sIn;

        private static synchronized void read(byte[] bytes) {
            try {
                if (sIn == null) sIn = new java.io.FileInputStream("/dev/urandom");
                for (int n = 0; n < bytes.length; ) {
                    int r = sIn.read(bytes, n, bytes.length - n);
                    if (r <= 0) throw new java.io.IOException("/dev/urandom: end of file");
                    n += r;
                }
            } catch (java.io.IOException e) {
                throw new java.security.ProviderException(e);
            }
        }

        @Override
        protected void engineSetSeed(byte[] seed) {
        }

        @Override
        protected void engineNextBytes(byte[] bytes) {
            read(bytes);
        }

        @Override
        protected byte[] engineGenerateSeed(int numBytes) {
            byte[] seed = new byte[numBytes];
            read(seed);
            return seed;
        }
    }

    /** Rewrites the configured provider list as described above. */
    public static void install() {
        List<String> kept = new ArrayList<>();
        kept.add(Digests.class.getName());
        for (int i = 1; ; i++) {
            String name = Security.getProperty("security.provider." + i);
            if (name == null) break;
            String className = name.trim().split("\\s+")[0];
            try {
                Class.forName(className, false, Digests.class.getClassLoader());
                kept.add(name);
            } catch (ClassNotFoundException e) {
                Main.log("security provider not on the boot class path: " + className);
            }
        }
        for (int i = 1; Security.getProperty("security.provider." + i) != null; i++) {
            Security.setProperty("security.provider." + i, "");
        }
        for (int i = 0; i < kept.size(); i++) {
            Security.setProperty("security.provider." + (i + 1), kept.get(i));
        }
    }

    /** A Merkle–Damgård digest over 64-byte blocks. */
    private abstract static class BlockDigest extends MessageDigestSpi {
        private final byte[] block = new byte[64];
        private int fill;
        private long length;
        private final boolean bigEndian;

        BlockDigest(boolean bigEndian) {
            this.bigEndian = bigEndian;
        }

        abstract void compress(byte[] b);

        abstract void reset();

        abstract byte[] output();

        @Override
        protected void engineUpdate(byte input) {
            block[fill++] = input;
            length++;
            if (fill == 64) {
                compress(block);
                fill = 0;
            }
        }

        @Override
        protected void engineUpdate(byte[] input, int offset, int len) {
            for (int i = 0; i < len; i++) engineUpdate(input[offset + i]);
        }

        @Override
        protected byte[] engineDigest() {
            long bits = length * 8;
            engineUpdate((byte) 0x80);
            while (fill != 56) engineUpdate((byte) 0);
            for (int i = 0; i < 8; i++) {
                int shift = bigEndian ? 56 - 8 * i : 8 * i;
                engineUpdate((byte) (bits >>> shift));
            }
            byte[] out = output();
            engineReset();
            return out;
        }

        @Override
        protected void engineReset() {
            fill = 0;
            length = 0;
            reset();
        }

        static int be(byte[] b, int i) {
            return (b[i] & 0xff) << 24 | (b[i + 1] & 0xff) << 16 | (b[i + 2] & 0xff) << 8
                    | (b[i + 3] & 0xff);
        }

        static int le(byte[] b, int i) {
            return (b[i + 3] & 0xff) << 24 | (b[i + 2] & 0xff) << 16 | (b[i + 1] & 0xff) << 8
                    | (b[i] & 0xff);
        }

        static byte[] words(int[] h, boolean bigEndian) {
            byte[] out = new byte[h.length * 4];
            for (int i = 0; i < h.length; i++) {
                for (int j = 0; j < 4; j++) {
                    int shift = bigEndian ? 24 - 8 * j : 8 * j;
                    out[4 * i + j] = (byte) (h[i] >>> shift);
                }
            }
            return out;
        }
    }

    /** SHA-1 (FIPS 180-4). */
    public static final class Sha1 extends BlockDigest {
        private final int[] h = new int[5];
        private final int[] w = new int[80];

        public Sha1() {
            super(true);
            reset();
        }

        @Override
        void reset() {
            h[0] = 0x67452301;
            h[1] = 0xefcdab89;
            h[2] = 0x98badcfe;
            h[3] = 0x10325476;
            h[4] = 0xc3d2e1f0;
        }

        @Override
        void compress(byte[] b) {
            for (int i = 0; i < 16; i++) w[i] = be(b, 4 * i);
            for (int i = 16; i < 80; i++) {
                w[i] = Integer.rotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            }
            int a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
            for (int i = 0; i < 80; i++) {
                int f, k;
                if (i < 20) {
                    f = (bb & c) | (~bb & d);
                    k = 0x5a827999;
                } else if (i < 40) {
                    f = bb ^ c ^ d;
                    k = 0x6ed9eba1;
                } else if (i < 60) {
                    f = (bb & c) | (bb & d) | (c & d);
                    k = 0x8f1bbcdc;
                } else {
                    f = bb ^ c ^ d;
                    k = 0xca62c1d6;
                }
                int t = Integer.rotateLeft(a, 5) + f + e + k + w[i];
                e = d;
                d = c;
                c = Integer.rotateLeft(bb, 30);
                bb = a;
                a = t;
            }
            h[0] += a;
            h[1] += bb;
            h[2] += c;
            h[3] += d;
            h[4] += e;
        }

        @Override
        byte[] output() {
            return words(h, true);
        }

        @Override
        protected int engineGetDigestLength() {
            return 20;
        }
    }

    /** SHA-256 (FIPS 180-4). */
    public static final class Sha256 extends BlockDigest {
        private static final int[] K = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
            0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
            0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
            0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
            0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2,
        };
        private final int[] h = new int[8];
        private final int[] w = new int[64];

        public Sha256() {
            super(true);
            reset();
        }

        @Override
        void reset() {
            h[0] = 0x6a09e667;
            h[1] = 0xbb67ae85;
            h[2] = 0x3c6ef372;
            h[3] = 0xa54ff53a;
            h[4] = 0x510e527f;
            h[5] = 0x9b05688c;
            h[6] = 0x1f83d9ab;
            h[7] = 0x5be0cd19;
        }

        @Override
        void compress(byte[] b) {
            for (int i = 0; i < 16; i++) w[i] = be(b, 4 * i);
            for (int i = 16; i < 64; i++) {
                int s0 = Integer.rotateRight(w[i - 15], 7) ^ Integer.rotateRight(w[i - 15], 18)
                        ^ (w[i - 15] >>> 3);
                int s1 = Integer.rotateRight(w[i - 2], 17) ^ Integer.rotateRight(w[i - 2], 19)
                        ^ (w[i - 2] >>> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }
            int a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
            for (int i = 0; i < 64; i++) {
                int s1 = Integer.rotateRight(e, 6) ^ Integer.rotateRight(e, 11)
                        ^ Integer.rotateRight(e, 25);
                int ch = (e & f) ^ (~e & g);
                int t1 = hh + s1 + ch + K[i] + w[i];
                int s0 = Integer.rotateRight(a, 2) ^ Integer.rotateRight(a, 13)
                        ^ Integer.rotateRight(a, 22);
                int maj = (a & bb) ^ (a & c) ^ (bb & c);
                int t2 = s0 + maj;
                hh = g;
                g = f;
                f = e;
                e = d + t1;
                d = c;
                c = bb;
                bb = a;
                a = t1 + t2;
            }
            h[0] += a;
            h[1] += bb;
            h[2] += c;
            h[3] += d;
            h[4] += e;
            h[5] += f;
            h[6] += g;
            h[7] += hh;
        }

        @Override
        byte[] output() {
            return words(h, true);
        }

        @Override
        protected int engineGetDigestLength() {
            return 32;
        }
    }

    /** MD5 (RFC 1321). */
    public static final class Md5 extends BlockDigest {
        private static final int[] S = {
            7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
            5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
            6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
        };
        private static final int[] T = new int[64];

        static {
            for (int i = 0; i < 64; i++) T[i] = (int) (long) Math.floor(Math.abs(Math.sin(i + 1)) * 4294967296.0);
        }

        private final int[] h = new int[4];
        private final int[] x = new int[16];

        public Md5() {
            super(false);
            reset();
        }

        @Override
        void reset() {
            h[0] = 0x67452301;
            h[1] = 0xefcdab89;
            h[2] = 0x98badcfe;
            h[3] = 0x10325476;
        }

        @Override
        void compress(byte[] b) {
            for (int i = 0; i < 16; i++) x[i] = le(b, 4 * i);
            int a = h[0], bb = h[1], c = h[2], d = h[3];
            for (int i = 0; i < 64; i++) {
                int f, g;
                if (i < 16) {
                    f = (bb & c) | (~bb & d);
                    g = i;
                } else if (i < 32) {
                    f = (d & bb) | (~d & c);
                    g = (5 * i + 1) % 16;
                } else if (i < 48) {
                    f = bb ^ c ^ d;
                    g = (3 * i + 5) % 16;
                } else {
                    f = c ^ (bb | ~d);
                    g = (7 * i) % 16;
                }
                int t = d;
                d = c;
                c = bb;
                bb = bb + Integer.rotateLeft(a + f + T[i] + x[g], S[i]);
                a = t;
            }
            h[0] += a;
            h[1] += bb;
            h[2] += c;
            h[3] += d;
        }

        @Override
        byte[] output() {
            return words(h, false);
        }

        @Override
        protected int engineGetDigestLength() {
            return 16;
        }
    }

    /**
     * CertificateFactory X.509: DER or PEM certificates, parsed by libcore's
     * sun.security.x509.X509CertImpl. No CRLs and no cert paths.
     */
    public static final class X509 extends java.security.cert.CertificateFactorySpi {
        private static final String BEGIN = "-----BEGIN CERTIFICATE-----";
        private static final String END = "-----END CERTIFICATE-----";

        @Override
        public java.security.cert.Certificate engineGenerateCertificate(java.io.InputStream in)
                throws java.security.cert.CertificateException {
            java.util.List<java.security.cert.Certificate> all = parse(in, 1);
            if (all.isEmpty()) {
                throw new java.security.cert.CertificateException("no certificate");
            }
            return all.get(0);
        }

        @Override
        public java.util.Collection<? extends java.security.cert.Certificate>
                engineGenerateCertificates(java.io.InputStream in)
                throws java.security.cert.CertificateException {
            return parse(in, Integer.MAX_VALUE);
        }

        @Override
        public java.security.cert.CRL engineGenerateCRL(java.io.InputStream in)
                throws java.security.cert.CRLException {
            throw new java.security.cert.CRLException("CRLs are not supported");
        }

        @Override
        public java.util.Collection<? extends java.security.cert.CRL> engineGenerateCRLs(
                java.io.InputStream in) throws java.security.cert.CRLException {
            throw new java.security.cert.CRLException("CRLs are not supported");
        }

        private static java.util.List<java.security.cert.Certificate> parse(
                java.io.InputStream in, int max) throws java.security.cert.CertificateException {
            byte[] bytes;
            try {
                bytes = in.readAllBytes();
            } catch (java.io.IOException e) {
                throw new java.security.cert.CertificateException(e);
            }
            java.util.List<java.security.cert.Certificate> out = new ArrayList<>();
            String text = new String(bytes, java.nio.charset.StandardCharsets.ISO_8859_1);
            int at = text.indexOf(BEGIN);
            if (at < 0) {
                out.add(new sun.security.x509.X509CertImpl(bytes));  // DER
                return out;
            }
            while (at >= 0 && out.size() < max) {
                int end = text.indexOf(END, at);
                if (end < 0) break;
                String b64 = text.substring(at + BEGIN.length(), end).replaceAll("\\s", "");
                out.add(new sun.security.x509.X509CertImpl(
                        java.util.Base64.getDecoder().decode(b64)));
                at = text.indexOf(BEGIN, end);
            }
            return out;
        }
    }
}
