package com.gesher.app;

import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProtection;

import com.gesher.app.AndroidKeyStoreSecretKeys.Authorization;
import com.gesher.app.AndroidKeyStoreSecretKeys.StoredKey;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.math.BigInteger;
import java.security.InvalidAlgorithmParameterException;
import java.security.InvalidKeyException;
import java.security.InvalidParameterException;
import java.security.Key;
import java.security.KeyFactory;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.KeyPairGeneratorSpi;
import java.security.KeyStore;
import java.security.KeyStoreException;
import java.security.KeyStoreSpi;
import java.security.NoSuchAlgorithmException;
import java.security.PrivateKey;
import java.security.Provider;
import java.security.SecureRandom;
import java.security.Security;
import java.security.Signature;
import java.security.cert.Certificate;
import java.security.cert.CertificateException;
import java.security.cert.CertificateFactory;
import java.security.spec.AlgorithmParameterSpec;
import java.security.spec.ECGenParameterSpec;
import java.security.spec.PKCS8EncodedKeySpec;
import java.security.spec.RSAKeyGenParameterSpec;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Calendar;
import java.util.Collections;
import java.util.Date;
import java.util.Enumeration;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TimeZone;
import javax.crypto.KeyGeneratorSpi;
import javax.crypto.SecretKey;
import javax.crypto.spec.SecretKeySpec;
import javax.security.auth.x500.X500Principal;

/**
 * The "AndroidKeyStore" JCA provider, in the app's own process.
 *
 * <p>On a device the zygote installs android.security.keystore2.AndroidKeyStoreProvider
 * (ZygoteInit.warmUpJcaProviders), whose keys live in the keystore2 service and KeyMint,
 * and are used through the provider's own Cipher, Signature and Mac. Gesher has no zygote
 * and no keystore2 service, so KeyStore.getInstance("AndroidKeyStore") failed with
 * "AndroidKeyStore not found" (ForgeRock's SecretKeyEncryptor, androidx.security, Tink).
 *
 * <p>This provider keeps the same API over software keys: KeyStore "AndroidKeyStore",
 * KeyGenerator AES and HmacSHA* and KeyPairGenerator RSA and EC, initialized with a
 * KeyGenParameterSpec (or the older KeyPairGeneratorSpec). New symmetric keys are
 * JCA handles. The provider checks their stored authorizations, then delegates the
 * requested AES or HMAC operation to the platform crypto provider. Older symmetric
 * keys have no saved authorizations. RSA and EC keys still use the platform JCA
 * provider directly and get a self-signed certificate.
 *
 * <p>The keys of an app are in its data directory ({@value #FILE}) without encryption
 * or a hardware or user-authentication binding. The handles keep raw symmetric keys
 * out of the public JCA encoding API. They are not an OS isolation boundary. Before
 * the app's Application exists, keys are kept in memory and written once it does.
 */
public final class AndroidKeyStore extends Provider {
    static final String NAME = "AndroidKeyStore";
    static final String FILE = ".gesher-keystore";

    public AndroidKeyStore() {
        super(NAME, 1.0, "Gesher's AndroidKeyStore: software keys in the app's data directory");
        put("KeyStore." + NAME, Spi.class.getName());
        put("KeyGenerator.AES", Aes.class.getName());
        put("KeyGenerator.HmacSHA1", HmacSha1.class.getName());
        put("KeyGenerator.HmacSHA224", HmacSha224.class.getName());
        put("KeyGenerator.HmacSHA256", HmacSha256.class.getName());
        put("KeyGenerator.HmacSHA384", HmacSha384.class.getName());
        put("KeyGenerator.HmacSHA512", HmacSha512.class.getName());
        put("KeyPairGenerator.RSA", Rsa.class.getName());
        put("KeyPairGenerator.EC", Ec.class.getName());
        AndroidKeyStoreSecretKeys.registerFactories(this);
    }

    /** Handles opaque keys ahead of providers which probe their key encoding. */
    public static final class CryptoOperations extends Provider {
        public CryptoOperations() {
            super("AndroidKeyStoreBCWorkaround", 1.0, "Gesher's AndroidKeyStore operations");
            AndroidKeyStoreSecretKeys.registerOperations(this);
        }
    }

    /**
     * Match Android's provider order. Conscrypt filters symmetric keys by format.
     * Bouncy Castle can attempt to read an opaque key before our SPI if it comes
     * first. Libcore would then report its failure instead of our checked rejection.
     * Keep the providers configured by class name so boot can still load them lazily.
     */
    static void configure(List<String> configured) {
        boolean writeProperties = configured == null;
        if (writeProperties) {
            configured = new ArrayList<>();
            for (int i = 1; ; i++) {
                String provider = Security.getProperty("security.provider." + i);
                if (provider == null || provider.isBlank()) break;
                configured.add(provider);
            }
        }
        String operations = CryptoOperations.class.getName();
        if (!configured.contains(operations)) {
            int index = configured.size();
            for (int i = 0; i < configured.size(); i++) {
                String name = configured.get(i).trim().split("\\s+", 2)[0];
                if ("com.android.org.bouncycastle.jce.provider.BouncyCastleProvider".equals(name)) {
                    index = i;
                    break;
                }
            }
            configured.add(index, operations);
        }
        String className = AndroidKeyStore.class.getName();
        if (!configured.contains(className)) configured.add(className);
        if (writeProperties) {
            for (int i = 0; i < configured.size(); i++) {
                Security.setProperty("security.provider." + (i + 1), configured.get(i));
            }
        }
    }

    // --- the keys --------------------------------------------------------

    private static final int SECRET = 1;
    private static final int PRIVATE = 2;
    private static final int TRUSTED = 3;
    private static final int LEGACY_MAGIC = 0x474b5331;  // "GKS1"
    private static final int MAGIC = 0x474b5332;  // "GKS2", optional symmetric authorization

    /** One alias: a secret key, a private key and its chain, or a trusted certificate. */
    static final class Entry {
        final int kind;
        final Key key;               // SECRET, PRIVATE
        final Certificate[] chain;   // PRIVATE: the key's chain; TRUSTED: the certificate
        final Date created;
        final Authorization authorization;

        Entry(int kind, Key key, Certificate[] chain, Date created) {
            this(kind, key, chain, created, null);
        }

        Entry(int kind, Key key, Certificate[] chain, Date created, Authorization authorization) {
            this.kind = kind;
            this.key = key;
            this.chain = chain;
            this.created = created;
            this.authorization = authorization;
        }

        Key appKey(String alias) {
            return authorization == null ? key
                    : new StoredKey(alias, authorization.id, key.getAlgorithm());
        }
    }

    /**
     * The process's keys, shared by every KeyStore, KeyGenerator and KeyPairGenerator of
     * this provider (on a device, keystore2's keys of the app's uid). Read again when
     * another process of the app changed the file.
     */
    private static final class Store {
        private static Map<String, Entry> sEntries = new LinkedHashMap<>();
        private static File sFile;     // the file read, null before the app's data directory
        private static long sStamp;
        private static long sLength;

        private static File file() {
            try {
                android.app.Application app = android.app.ActivityThread.currentApplication();
                File dir = app != null ? app.getDataDir() : null;
                return dir != null ? new File(dir, FILE) : null;
            } catch (RuntimeException e) {
                return null;
            }
        }

        /** Brings the keys in memory up to date with the file. */
        private static void sync() {
            File f = file();
            if (f == null) return;  // no app yet (or the system server): memory only
            if (sFile == null) {
                // The app's data directory is known now: the file's keys, then the ones made
                // before (they win).
                Map<String, Entry> merged = read(f);
                boolean pending = !sEntries.isEmpty();
                merged.putAll(sEntries);
                sEntries = merged;
                sFile = f;
                if (pending) write();
                else stamp();
            } else if (sFile.lastModified() != sStamp || sFile.length() != sLength) {
                sEntries = read(sFile);
                stamp();
            }
        }

        private static void stamp() {
            sStamp = sFile.lastModified();
            sLength = sFile.length();
        }

        static synchronized Entry get(String alias) {
            sync();
            return sEntries.get(alias);
        }

        static synchronized List<String> aliases() {
            sync();
            return new ArrayList<>(sEntries.keySet());
        }

        static synchronized void put(String alias, Entry entry) throws KeyStoreException {
            sync();
            encode(entry);  // only what can be written
            sEntries.put(alias, entry);
            write();
        }

        static synchronized void remove(String alias) {
            sync();
            if (sEntries.remove(alias) != null) write();
        }

        private static Map<String, Entry> read(File f) {
            Map<String, Entry> out = new LinkedHashMap<>();
            if (!f.exists()) return out;
            try (DataInputStream in = new DataInputStream(new java.io.BufferedInputStream(
                    new FileInputStream(f)))) {
                int magic = in.readInt();
                if (magic != MAGIC && magic != LEGACY_MAGIC) {
                    throw new IOException("not a Gesher key store");
                }
                int n = in.readInt();
                CertificateFactory x509 = null;
                for (int i = 0; i < n; i++) {
                    String alias = in.readUTF();
                    int kind = in.readByte();
                    Date created = new Date(in.readLong());
                    String algorithm = in.readUTF();
                    byte[] encoded = bytes(in);
                    Certificate[] chain = new Certificate[in.readInt()];
                    for (int c = 0; c < chain.length; c++) {
                        if (x509 == null) x509 = CertificateFactory.getInstance("X.509");
                        chain[c] = x509.generateCertificate(new ByteArrayInputStream(bytes(in)));
                    }
                    Key key = null;
                    if (kind == SECRET) {
                        key = new SecretKeySpec(encoded, algorithm);
                    } else if (kind == PRIVATE) {
                        key = KeyFactory.getInstance(algorithm)
                                .generatePrivate(new PKCS8EncodedKeySpec(encoded));
                    }
                    Authorization authorization = null;
                    if (magic == MAGIC && in.readBoolean()) {
                        if (kind != SECRET) throw new IOException("Invalid symmetric authorization");
                        authorization = Authorization.read(in);
                    }
                    out.put(alias, new Entry(kind, key, chain, created, authorization));
                }
            } catch (IOException | java.security.GeneralSecurityException e) {
                log("cannot read " + f + ", its keys are not used: " + e);
            }
            return out;
        }

        private static void write() {
            if (sFile == null) return;
            File tmp = new File(sFile.getPath() + ".tmp");
            try {
                try (DataOutputStream out = new DataOutputStream(new java.io.BufferedOutputStream(
                        new FileOutputStream(tmp)))) {
                    out.writeInt(MAGIC);
                    out.writeInt(sEntries.size());
                    for (Map.Entry<String, Entry> e : sEntries.entrySet()) {
                        Entry entry = e.getValue();
                        out.writeUTF(e.getKey());
                        out.writeByte(entry.kind);
                        out.writeLong(entry.created.getTime());
                        out.writeUTF(entry.key != null ? entry.key.getAlgorithm() : "");
                        byte[][] parts = encode(entry);
                        writeBytes(out, parts[0]);
                        out.writeInt(parts.length - 1);
                        for (int i = 1; i < parts.length; i++) writeBytes(out, parts[i]);
                        out.writeBoolean(entry.authorization != null);
                        if (entry.authorization != null) entry.authorization.write(out);
                    }
                }
                try {
                    java.nio.file.Files.move(tmp.toPath(), sFile.toPath(),
                            java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                } catch (IOException | RuntimeException e) {
                    if (!sFile.delete() && sFile.exists()) throw e;
                    if (!tmp.renameTo(sFile)) throw new IOException("cannot rename " + tmp);
                }
                stamp();
            } catch (IOException | KeyStoreException e) {
                log("cannot write " + sFile + ", the keys stay in memory: " + e);
            }
        }

        /** The key's encoding, then the chain's. */
        private static byte[][] encode(Entry entry) throws KeyStoreException {
            byte[][] parts = new byte[1 + (entry.chain != null ? entry.chain.length : 0)][];
            parts[0] = new byte[0];
            if (entry.key != null) {
                parts[0] = entry.key.getEncoded();
                if (parts[0] == null) {
                    throw new KeyStoreException("the key has no encoding: "
                            + entry.key.getClass().getName());
                }
            }
            for (int i = 1; i < parts.length; i++) {
                try {
                    parts[i] = entry.chain[i - 1].getEncoded();
                } catch (java.security.cert.CertificateEncodingException e) {
                    throw new KeyStoreException(e);
                }
            }
            return parts;
        }

        private static byte[] bytes(DataInputStream in) throws IOException {
            byte[] b = new byte[in.readInt()];
            in.readFully(b);
            return b;
        }

        private static void writeBytes(DataOutputStream out, byte[] b) throws IOException {
            out.writeInt(b.length);
            out.write(b);
        }
    }

    static Entry resolveSecret(StoredKey key) throws InvalidKeyException {
        Entry entry = Store.get(key.alias);
        if (entry == null || entry.authorization == null || !entry.authorization.id.equals(key.id)) {
            throw new InvalidKeyException("AndroidKeyStore key no longer exists");
        }
        return entry;
    }

    static void log(String message) {
        System.err.println("gesher-keystore: " + message);
    }

    // --- KeyStore --------------------------------------------------------

    /** KeyStore "AndroidKeyStore": load(null) only, the keys are the Store's. */
    public static final class Spi extends KeyStoreSpi {
        @Override
        public Key engineGetKey(String alias, char[] password) {
            Entry e = alias != null ? Store.get(alias) : null;
            return e != null ? e.appKey(alias) : null;
        }

        @Override
        public Certificate[] engineGetCertificateChain(String alias) {
            Entry e = alias != null ? Store.get(alias) : null;
            return e != null && e.kind == PRIVATE && e.chain.length > 0 ? e.chain.clone() : null;
        }

        @Override
        public Certificate engineGetCertificate(String alias) {
            Entry e = alias != null ? Store.get(alias) : null;
            return e != null && e.chain != null && e.chain.length > 0 ? e.chain[0] : null;
        }

        @Override
        public Date engineGetCreationDate(String alias) {
            Entry e = alias != null ? Store.get(alias) : null;
            return e != null ? new Date(e.created.getTime()) : null;
        }

        @Override
        public void engineSetKeyEntry(String alias, Key key, char[] password,
                Certificate[] chain) throws KeyStoreException {
            if (alias == null || key == null) throw new KeyStoreException("alias and key needed");
            if (key instanceof SecretKey) {
                importSecret(alias, (SecretKey) key, null);
            } else if (key instanceof PrivateKey) {
                if (chain == null || chain.length == 0) {
                    throw new KeyStoreException("a private key needs its certificate chain");
                }
                Store.put(alias, new Entry(PRIVATE, key, chain.clone(), new Date()));
            } else {
                throw new KeyStoreException("unsupported key: " + key.getClass().getName());
            }
        }

        @Override
        public void engineSetKeyEntry(String alias, byte[] key, Certificate[] chain)
                throws KeyStoreException {
            throw new KeyStoreException("encrypted keys are not supported");
        }

        @Override
        public void engineSetCertificateEntry(String alias, Certificate cert)
                throws KeyStoreException {
            if (alias == null || cert == null) throw new KeyStoreException("alias and cert needed");
            Entry old = Store.get(alias);
            if (old != null && old.kind != TRUSTED) {
                throw new KeyStoreException(alias + " is a key entry");
            }
            Store.put(alias, new Entry(TRUSTED, null, new Certificate[] {cert}, new Date()));
        }

        @Override
        public void engineDeleteEntry(String alias) {
            if (alias != null) Store.remove(alias);
        }

        @Override
        public Enumeration<String> engineAliases() {
            return Collections.enumeration(Store.aliases());
        }

        @Override
        public boolean engineContainsAlias(String alias) {
            return alias != null && Store.get(alias) != null;
        }

        @Override
        public int engineSize() {
            return Store.aliases().size();
        }

        @Override
        public boolean engineIsKeyEntry(String alias) {
            Entry e = alias != null ? Store.get(alias) : null;
            return e != null && e.kind != TRUSTED;
        }

        @Override
        public boolean engineIsCertificateEntry(String alias) {
            Entry e = alias != null ? Store.get(alias) : null;
            return e != null && e.kind == TRUSTED;
        }

        @Override
        public String engineGetCertificateAlias(Certificate cert) {
            if (cert == null) return null;
            for (String alias : Store.aliases()) {
                Entry e = Store.get(alias);
                if (e != null && e.chain != null && e.chain.length > 0 && cert.equals(e.chain[0])) {
                    return alias;
                }
            }
            return null;
        }

        @Override
        public void engineStore(OutputStream stream, char[] password) {
            throw new UnsupportedOperationException("Can not serialize AndroidKeyStore to OutputStream");
        }

        @Override
        public void engineLoad(InputStream stream, char[] password) {
            if (stream != null) throw new IllegalArgumentException("InputStream not supported");
            if (password != null) throw new IllegalArgumentException("password not supported");
        }

        @Override
        public void engineLoad(KeyStore.LoadStoreParameter param) {
            // AndroidKeyStoreLoadStoreParameter (a namespace) or null: one key store here.
        }

        /** Entries without a password, as Android's (KeyProtection is a ProtectionParameter). */
        @Override
        public KeyStore.Entry engineGetEntry(String alias, KeyStore.ProtectionParameter param) {
            Entry e = alias != null ? Store.get(alias) : null;
            if (e == null) return null;
            switch (e.kind) {
                case SECRET:
                    return new KeyStore.SecretKeyEntry((SecretKey) e.appKey(alias));
                case PRIVATE:
                    return new KeyStore.PrivateKeyEntry((PrivateKey) e.key, e.chain.clone());
                default:
                    return new KeyStore.TrustedCertificateEntry(e.chain[0]);
            }
        }

        /** Imports a key or certificate. Symmetric imports require KeyProtection. */
        @Override
        public void engineSetEntry(String alias, KeyStore.Entry entry,
                KeyStore.ProtectionParameter param) throws KeyStoreException {
            if (entry instanceof KeyStore.SecretKeyEntry) {
                importSecret(alias, ((KeyStore.SecretKeyEntry) entry).getSecretKey(), param);
            } else if (entry instanceof KeyStore.PrivateKeyEntry) {
                KeyStore.PrivateKeyEntry p = (KeyStore.PrivateKeyEntry) entry;
                engineSetKeyEntry(alias, p.getPrivateKey(), null, p.getCertificateChain());
            } else if (entry instanceof KeyStore.TrustedCertificateEntry) {
                engineSetCertificateEntry(alias,
                        ((KeyStore.TrustedCertificateEntry) entry).getTrustedCertificate());
            } else {
                throw new KeyStoreException("unsupported entry: " + entry);
            }
        }

        private static void importSecret(String alias, SecretKey key,
                KeyStore.ProtectionParameter param) throws KeyStoreException {
            if (alias == null) throw new KeyStoreException("alias needed");
            if (key instanceof StoredKey stored) {
                if (!alias.equals(stored.alias) || param != null) {
                    throw new KeyStoreException("Cannot move or reauthorize an AndroidKeyStore key");
                }
                try {
                    resolveSecret(stored);
                } catch (InvalidKeyException e) {
                    throw new KeyStoreException(e);
                }
                return;
            }
            if (!(param instanceof KeyProtection protection)) {
                throw new KeyStoreException("Secret key import requires KeyProtection");
            }
            byte[] encoded = key.getEncoded();
            if (!"RAW".equals(key.getFormat()) || encoded == null) {
                throw new KeyStoreException("Expected a RAW secret key");
            }
            try {
                String algorithm = AndroidKeyStoreSecretKeys.canonicalAlgorithm(key.getAlgorithm());
                if (encoded.length > 64
                        || !AndroidKeyStoreSecretKeys.validSize(algorithm, encoded.length * 8)) {
                    throw new InvalidAlgorithmParameterException("Unsupported secret key size");
                }
                Authorization authorization = AndroidKeyStoreSecretKeys.imported(
                        algorithm, encoded.length * 8, protection);
                Store.put(alias, new Entry(SECRET, new SecretKeySpec(encoded, algorithm),
                        new Certificate[0], new Date(), authorization));
            } catch (InvalidAlgorithmParameterException e) {
                throw new KeyStoreException(e);
            }
        }

        @Override
        public boolean engineEntryInstanceOf(String alias,
                Class<? extends KeyStore.Entry> entryClass) {
            Entry e = alias != null ? Store.get(alias) : null;
            if (e == null) return false;
            switch (e.kind) {
                case SECRET: return entryClass == KeyStore.SecretKeyEntry.class;
                case PRIVATE: return entryClass == KeyStore.PrivateKeyEntry.class;
                default: return entryClass == KeyStore.TrustedCertificateEntry.class;
            }
        }
    }

    // --- KeyGenerator ----------------------------------------------------

    /** KeyGenerator: a random key of the spec's size, stored under the spec's alias. */
    abstract static class SecretKeyGenerator extends KeyGeneratorSpi {
        private final String algorithm;
        private final int defaultBits;
        private String alias;
        private Authorization authorization;
        private SecureRandom random;

        SecretKeyGenerator(String algorithm, int defaultBits) {
            this.algorithm = algorithm;
            this.defaultBits = defaultBits;
        }

        @Override
        protected void engineInit(SecureRandom random) {
            throw new UnsupportedOperationException("Cannot initialize without a "
                    + "android.security.keystore.KeyGenParameterSpec parameter");
        }

        @Override
        protected void engineInit(int keySize, SecureRandom random) {
            throw new UnsupportedOperationException("Cannot initialize without a "
                    + "android.security.keystore.KeyGenParameterSpec parameter");
        }

        @Override
        protected void engineInit(AlgorithmParameterSpec params, SecureRandom random)
                throws InvalidAlgorithmParameterException {
            // A rejected spec invalidates the previous initialization as on Android.
            alias = null;
            this.random = null;
            if (!(params instanceof KeyGenParameterSpec spec)) {
                throw new InvalidAlgorithmParameterException("Cannot initialize without a "
                        + KeyGenParameterSpec.class.getName() + " parameter");
            }
            int size = spec.getKeySize() == -1 ? defaultBits : spec.getKeySize();
            if (!AndroidKeyStoreSecretKeys.validSize(algorithm, size)) {
                throw new InvalidAlgorithmParameterException("Unsupported key size: " + size);
            }
            authorization = AndroidKeyStoreSecretKeys.generated(algorithm, size, spec);
            alias = spec.getKeystoreAlias();
            this.random = random != null ? random : new SecureRandom();
        }

        @Override
        protected SecretKey engineGenerateKey() {
            if (alias == null) throw new IllegalStateException("Not initialized");
            // Repeated calls on an initialized generator must create distinct alias entries.
            Authorization policy = authorization.forNewKey();
            byte[] raw = new byte[policy.bits / 8];
            random.nextBytes(raw);
            Entry entry = new Entry(SECRET, new SecretKeySpec(raw, algorithm),
                    new Certificate[0], new Date(), policy);
            try {
                Store.put(alias, entry);
            } catch (KeyStoreException e) {
                throw new java.security.ProviderException("Failed to store " + alias, e);
            }
            return (SecretKey) entry.appKey(alias);
        }
    }

    public static final class Aes extends SecretKeyGenerator {
        public Aes() {
            super("AES", 128);
        }
    }

    public static final class HmacSha1 extends SecretKeyGenerator {
        public HmacSha1() {
            super("HmacSHA1", 160);
        }
    }

    public static final class HmacSha224 extends SecretKeyGenerator {
        public HmacSha224() {
            super("HmacSHA224", 224);
        }
    }

    public static final class HmacSha256 extends SecretKeyGenerator {
        public HmacSha256() {
            super("HmacSHA256", 256);
        }
    }

    public static final class HmacSha384 extends SecretKeyGenerator {
        public HmacSha384() {
            super("HmacSHA384", 384);
        }
    }

    public static final class HmacSha512 extends SecretKeyGenerator {
        public HmacSha512() {
            super("HmacSHA512", 512);
        }
    }

    // --- KeyPairGenerator ------------------------------------------------

    /**
     * KeyPairGenerator: a pair from another provider (Conscrypt's), stored under the
     * spec's alias with a self-signed certificate.
     */
    abstract static class PairGenerator extends KeyPairGeneratorSpi {
        private final String algorithm;
        private String alias;
        private int bits;
        private AlgorithmParameterSpec keyParams;
        private X500Principal subject;
        private BigInteger serial;
        private Date notBefore;
        private Date notAfter;
        private SecureRandom random;

        PairGenerator(String algorithm) {
            this.algorithm = algorithm;
        }

        @Override
        public void initialize(int keysize, SecureRandom random) {
            throw new IllegalArgumentException("android.security.keystore.KeyGenParameterSpec"
                    + " or android.security.KeyPairGeneratorSpec required to initialize this"
                    + " KeyPairGenerator");
        }

        @Override
        @SuppressWarnings("deprecation")
        public void initialize(AlgorithmParameterSpec params, SecureRandom random)
                throws InvalidAlgorithmParameterException {
            if (params instanceof android.security.keystore.KeyGenParameterSpec) {
                android.security.keystore.KeyGenParameterSpec spec =
                        (android.security.keystore.KeyGenParameterSpec) params;
                alias = spec.getKeystoreAlias();
                bits = spec.getKeySize();
                keyParams = spec.getAlgorithmParameterSpec();
                subject = spec.getCertificateSubject();
                serial = spec.getCertificateSerialNumber();
                notBefore = spec.getCertificateNotBefore();
                notAfter = spec.getCertificateNotAfter();
            } else if (params instanceof android.security.KeyPairGeneratorSpec) {
                android.security.KeyPairGeneratorSpec spec =
                        (android.security.KeyPairGeneratorSpec) params;
                if (spec.getKeyType() != null && !algorithm.equalsIgnoreCase(spec.getKeyType())) {
                    throw new InvalidAlgorithmParameterException("Key type " + spec.getKeyType()
                            + " does not match " + algorithm);
                }
                alias = spec.getKeystoreAlias();
                bits = spec.getKeySize();
                keyParams = spec.getAlgorithmParameterSpec();
                subject = spec.getSubjectDN();
                serial = spec.getSerialNumber();
                notBefore = spec.getStartDate();
                notAfter = spec.getEndDate();
            } else {
                throw new InvalidAlgorithmParameterException(
                        "android.security.keystore.KeyGenParameterSpec or"
                        + " android.security.KeyPairGeneratorSpec required");
            }
            if (alias == null) throw new InvalidAlgorithmParameterException("no alias");
            if (subject == null) subject = new X500Principal("CN=fake");
            if (serial == null || serial.signum() <= 0) serial = BigInteger.ONE;
            if (notBefore == null) notBefore = new Date(0L);
            if (notAfter == null) notAfter = new Date(2461449600000L);  // 2048-01-01
            this.random = random != null ? random : new SecureRandom();
        }

        abstract AlgorithmParameterSpec keyGenParams(int bits, AlgorithmParameterSpec given)
                throws InvalidAlgorithmParameterException;

        abstract String signatureAlgorithm();

        abstract byte[] signatureAlgorithmId();

        @Override
        public KeyPair generateKeyPair() {
            if (alias == null) throw new IllegalStateException("Not initialized");
            try {
                KeyPairGenerator g = KeyPairGenerator.getInstance(algorithm, other("KeyPairGenerator", algorithm));
                g.initialize(keyGenParams(bits, keyParams), random);
                KeyPair pair = g.generateKeyPair();
                Certificate cert = selfSigned(pair);
                Store.put(alias, new Entry(PRIVATE, pair.getPrivate(), new Certificate[] {cert},
                        new Date()));
                return pair;
            } catch (java.security.GeneralSecurityException | IOException e) {
                throw new java.security.ProviderException("Failed to generate key pair " + alias, e);
            }
        }

        /** X.509 v3, issuer = subject, signed by the pair's own private key. */
        private Certificate selfSigned(KeyPair pair)
                throws java.security.GeneralSecurityException, IOException {
            byte[] algId = signatureAlgorithmId();
            byte[] name = subject.getEncoded();
            byte[] tbs = Der.seq(
                    Der.tlv(0xa0, Der.integer(BigInteger.valueOf(2))),  // [0] version v3
                    Der.integer(serial),
                    algId,
                    name,
                    Der.seq(Der.time(notBefore), Der.time(notAfter)),
                    name,
                    pair.getPublic().getEncoded());
            Signature s = Signature.getInstance(signatureAlgorithm());
            s.initSign(pair.getPrivate());
            s.update(tbs);
            byte[] sig = s.sign();
            byte[] bits = new byte[sig.length + 1];  // no unused bits
            System.arraycopy(sig, 0, bits, 1, sig.length);
            byte[] der = Der.seq(tbs, algId, Der.tlv(0x03, bits));
            return CertificateFactory.getInstance("X.509")
                    .generateCertificate(new ByteArrayInputStream(der));
        }
    }

    /** The first provider other than this one with the service. */
    static Provider other(String type, String algorithm) throws NoSuchAlgorithmException {
        Provider[] all = Security.getProviders(type + "." + algorithm);
        if (all != null) {
            for (Provider p : all) {
                if (!(p instanceof AndroidKeyStore)) return p;
            }
        }
        throw new NoSuchAlgorithmException(type + " " + algorithm + " not available");
    }

    public static final class Rsa extends PairGenerator {
        public Rsa() {
            super("RSA");
        }

        @Override
        AlgorithmParameterSpec keyGenParams(int bits, AlgorithmParameterSpec given)
                throws InvalidAlgorithmParameterException {
            BigInteger e = RSAKeyGenParameterSpec.F4;
            if (given instanceof RSAKeyGenParameterSpec) {
                RSAKeyGenParameterSpec r = (RSAKeyGenParameterSpec) given;
                if (bits <= 0) bits = r.getKeysize();
                e = r.getPublicExponent();
            } else if (given != null) {
                throw new InvalidAlgorithmParameterException("RSA: " + given.getClass().getName());
            }
            return new RSAKeyGenParameterSpec(bits > 0 ? bits : 2048, e);
        }

        @Override
        String signatureAlgorithm() {
            return "SHA256withRSA";
        }

        @Override
        byte[] signatureAlgorithmId() {
            // sha256WithRSAEncryption (1.2.840.113549.1.1.11), NULL parameters
            return Der.seq(Der.tlv(0x06, new byte[] {0x2a, (byte) 0x86, 0x48, (byte) 0x86,
                    (byte) 0xf7, 0x0d, 0x01, 0x01, 0x0b}), new byte[] {0x05, 0x00});
        }
    }

    public static final class Ec extends PairGenerator {
        public Ec() {
            super("EC");
        }

        @Override
        AlgorithmParameterSpec keyGenParams(int bits, AlgorithmParameterSpec given)
                throws InvalidAlgorithmParameterException {
            if (given instanceof ECGenParameterSpec) return given;
            if (given != null) {
                throw new InvalidAlgorithmParameterException("EC: " + given.getClass().getName());
            }
            switch (bits <= 0 ? 256 : bits) {
                case 224: return new ECGenParameterSpec("secp224r1");
                case 256: return new ECGenParameterSpec("secp256r1");
                case 384: return new ECGenParameterSpec("secp384r1");
                case 521: return new ECGenParameterSpec("secp521r1");
                default: throw new InvalidParameterException("Unsupported EC key size: " + bits);
            }
        }

        @Override
        String signatureAlgorithm() {
            return "SHA256withECDSA";
        }

        @Override
        byte[] signatureAlgorithmId() {
            // ecdsa-with-SHA256 (1.2.840.10045.4.3.2), no parameters
            return Der.seq(Der.tlv(0x06, new byte[] {0x2a, (byte) 0x86, 0x48, (byte) 0xce,
                    0x3d, 0x04, 0x03, 0x02}));
        }
    }

    /** Just enough DER for a certificate. */
    private static final class Der {
        static byte[] tlv(int tag, byte[] value) {
            ByteArrayOutputStream out = new ByteArrayOutputStream(value.length + 6);
            out.write(tag);
            int n = value.length;
            if (n < 0x80) {
                out.write(n);
            } else {
                int bytes = n > 0xffffff ? 4 : n > 0xffff ? 3 : n > 0xff ? 2 : 1;
                out.write(0x80 | bytes);
                for (int i = bytes - 1; i >= 0; i--) out.write(n >>> (8 * i));
            }
            out.write(value, 0, n);
            return out.toByteArray();
        }

        static byte[] seq(byte[]... parts) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            for (byte[] p : parts) out.write(p, 0, p.length);
            return tlv(0x30, out.toByteArray());
        }

        static byte[] integer(BigInteger value) {
            return tlv(0x02, value.toByteArray());
        }

        /** UTCTime through 2049, GeneralizedTime after (RFC 5280 4.1.2.5). */
        static byte[] time(Date date) {
            Calendar c = Calendar.getInstance(TimeZone.getTimeZone("UTC"), Locale.US);
            c.setTime(date);
            int year = c.get(Calendar.YEAR);
            boolean utc = year >= 1950 && year < 2050;
            SimpleDateFormat f = new SimpleDateFormat(utc ? "yyMMddHHmmss'Z'"
                    : "yyyyMMddHHmmss'Z'", Locale.US);
            f.setTimeZone(TimeZone.getTimeZone("UTC"));
            return tlv(utc ? 0x17 : 0x18,
                    f.format(date).getBytes(java.nio.charset.StandardCharsets.US_ASCII));
        }
    }
}
