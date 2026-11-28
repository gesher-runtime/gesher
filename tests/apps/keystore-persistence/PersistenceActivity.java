package com.gesher.keystorepersist;

import android.app.Activity;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.os.Process;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyInfo;
import android.security.keystore.KeyProperties;
import android.util.Log;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.security.InvalidKeyException;
import java.security.KeyStore;
import java.util.Arrays;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.Mac;
import javax.crypto.SecretKey;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.GCMParameterSpec;

public final class PersistenceActivity extends Activity {
    private static final String PROVIDER = "AndroidKeyStore";
    private static final String TAG = "S04Persist";
    private static final byte[] MESSAGE = new byte[] {0x47, 0x35, 0x36, 0x50};

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        new Thread(() -> {
            try {
                SharedPreferences prefs = getSharedPreferences("phase", MODE_PRIVATE);
                int phase = prefs.getInt("phase", 0);
                int pid = Process.myPid();
                require(pid != prefs.getInt("pid", 0), "different process required");
                KeyStore store = KeyStore.getInstance(PROVIDER);
                store.load(null);
                switch (phase) {
                    case 0 -> seed();
                    case 1 -> verifyAndDelete(store);
                    case 2 -> require(!store.containsAlias("persist-aes") && !store.containsAlias("persist-mac"),
                            "deleted keys reappeared after process restart");
                    default -> throw new AssertionError("unexpected phase " + phase);
                }
                require(prefs.edit().putInt("phase", phase + 1).putInt("pid", pid).commit(), "persist phase");
                Log.i(TAG, "phase=" + phase + " pid=" + pid + " PASS");
            } catch (Exception | AssertionError error) {
                Log.e(TAG, "FAIL", error);
            } finally {
                runOnUiThread(this::finish);
            }
        }, "keystore-persistence").start();
    }

    private void seed() throws Exception {
        KeyGenerator aesGenerator = KeyGenerator.getInstance("AES", PROVIDER);
        aesGenerator.init(new KeyGenParameterSpec.Builder("persist-aes",
                KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setKeySize(128).setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build());
        SecretKey aes = aesGenerator.generateKey();
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, aes);
        byte[] ciphertext = cipher.doFinal(MESSAGE);
        byte[] iv = cipher.getIV();
        KeyGenerator macGenerator = KeyGenerator.getInstance("HmacSHA256", PROVIDER);
        macGenerator.init(new KeyGenParameterSpec.Builder("persist-mac", KeyProperties.PURPOSE_SIGN)
                .setKeySize(256).build());
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(macGenerator.generateKey());
        byte[] digest = mac.doFinal(MESSAGE);
        require(iv.length == 12 && ciphertext.length == 20 && digest.length == 32, "fixture lengths");
        try (DataOutputStream out = new DataOutputStream(openFileOutput("known-outputs", MODE_PRIVATE))) {
            out.write(iv);
            out.write(ciphertext);
            out.write(digest);
        }
    }

    private void verifyAndDelete(KeyStore store) throws Exception {
        SecretKey aes = (SecretKey) store.getKey("persist-aes", null);
        SecretKey hmac = (SecretKey) store.getKey("persist-mac", null);
        require(aes != null && hmac != null && aes.getEncoded() == null && hmac.getEncoded() == null,
                "both persisted key handles must remain opaque");
        KeyInfo info = (KeyInfo) SecretKeyFactory.getInstance("AES", PROVIDER).getKeySpec(aes, KeyInfo.class);
        require(info.getKeySize() == 128 && info.getOrigin() == KeyProperties.ORIGIN_GENERATED
                && Arrays.equals(info.getBlockModes(), new String[] {KeyProperties.BLOCK_MODE_GCM}),
                "persisted AES authorization");
        try {
            Cipher.getInstance("AES/CBC/NoPadding").init(Cipher.ENCRYPT_MODE, aes);
            throw new AssertionError("persisted GCM-only key accepted CBC");
        } catch (InvalidKeyException expected) { }
        byte[] iv, ciphertext, digest;
        try (DataInputStream in = new DataInputStream(openFileInput("known-outputs"))) {
            iv = in.readNBytes(12);
            ciphertext = in.readNBytes(20);
            digest = in.readNBytes(32);
            require(in.read() == -1, "no trailing fixture bytes");
        }
        Cipher decipher = Cipher.getInstance("AES/GCM/NoPadding");
        decipher.init(Cipher.DECRYPT_MODE, aes, new GCMParameterSpec(128, iv));
        require(Arrays.equals(MESSAGE, decipher.doFinal(ciphertext)), "decrypt original ciphertext after restart");
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(hmac);
        require(Arrays.equals(digest, mac.doFinal(MESSAGE)), "same HMAC after restart");
        store.deleteEntry("persist-aes");
        store.deleteEntry("persist-mac");
    }

    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
}
