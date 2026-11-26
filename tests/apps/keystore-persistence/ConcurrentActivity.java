/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.keystorepersist;

import android.app.Activity;
import android.os.Bundle;
import android.os.Process;
import android.os.SystemClock;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Log;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.security.Key;
import java.security.KeyStore;
import javax.crypto.KeyGenerator;
import javax.crypto.Mac;

/** Two live app processes publish different, real Keystore keys to one UID. */
public abstract class ConcurrentActivity extends Activity {
    private static final String PROVIDER = "AndroidKeyStore";
    private static final String TAG = "GesherKeystoreConcurrent";
    private static final int PER_WRITER = 16;

    public static final class WriterA extends ConcurrentActivity {
        @Override String writer() { return "a"; }
    }

    public static final class WriterB extends ConcurrentActivity {
        @Override String writer() { return "b"; }
    }

    public static final class Verify extends ConcurrentActivity {
        @Override String writer() { return null; }
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        new Thread(() -> {
            try {
                String writer = writer();
                if (writer == null) {
                    verify();
                } else {
                    write(writer);
                    Log.i(TAG, "writer=" + writer + " keys=" + PER_WRITER
                            + " pid=" + Process.myPid() + " PASS");
                }
            } catch (Exception | AssertionError failure) {
                Log.e(TAG, "FAIL", failure);
            } finally {
                runOnUiThread(this::finish);
            }
        }, "keystore-concurrency").start();
    }

    abstract String writer();

    private static String alias(String writer, int index) {
        return "concurrent-" + writer + "-" + index;
    }

    private void write(String writer) throws Exception {
        KeyGenerator generator = KeyGenerator.getInstance("HmacSHA256", PROVIDER);
        Path barrier = barrier();
        Files.createDirectories(barrier);
        Files.createFile(barrier.resolve(writer));
        waitFor(barrier.resolve("a".equals(writer) ? "b" : "a"));
        for (int i = 0; i < PER_WRITER; i++) {
            generator.init(new KeyGenParameterSpec.Builder(alias(writer, i), KeyProperties.PURPOSE_SIGN)
                    .setKeySize(256).build());
            if (generator.generateKey().getEncoded() != null) {
                throw new AssertionError("Published an exportable key");
            }
        }
        Path completed = barrier.resolve(writer + "-done");
        Files.write(completed, Integer.toString(Process.myPid()).getBytes(StandardCharsets.US_ASCII),
                StandardOpenOption.CREATE_NEW);
        Files.move(completed, barrier.resolve(writer + "-pid"), StandardCopyOption.ATOMIC_MOVE);
    }

    private Path barrier() {
        return getFilesDir().toPath().resolve("keystore-writers");
    }

    private static void waitFor(Path marker) throws Exception {
        long deadline = SystemClock.elapsedRealtime() + 20_000;
        while (!Files.exists(marker)) {
            if (SystemClock.elapsedRealtime() >= deadline) {
                throw new AssertionError("Missing writer marker " + marker.getFileName());
            }
            Thread.sleep(20);
        }
    }

    private void verify() throws Exception {
        Path writerA = barrier().resolve("a-pid");
        Path writerB = barrier().resolve("b-pid");
        waitFor(writerA);
        waitFor(writerB);
        int a = Integer.parseInt(new String(Files.readAllBytes(writerA), StandardCharsets.US_ASCII));
        int b = Integer.parseInt(new String(Files.readAllBytes(writerB), StandardCharsets.US_ASCII));
        int verifier = Process.myPid();
        if (a == b || a == verifier || b == verifier) throw new AssertionError("Expected three processes");
        KeyStore store = KeyStore.getInstance(PROVIDER);
        store.load(null);
        for (String writer : new String[] {"a", "b"}) {
            for (int i = 0; i < PER_WRITER; i++) {
                String alias = alias(writer, i);
                Key key = store.getKey(alias, null);
                if (key == null || key.getEncoded() != null) throw new AssertionError("Invalid key " + alias);
                Mac mac = Mac.getInstance("HmacSHA256");
                mac.init(key);
                if (mac.doFinal(new byte[] {1, 2, 3}).length != 32) {
                    throw new AssertionError("Invalid MAC " + alias);
                }
            }
        }
        Log.i(TAG, "verified=" + 2 * PER_WRITER + " writers=" + a + "," + b + " pid=" + verifier + " PASS");
    }
}
