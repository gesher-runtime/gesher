/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import java.io.File;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.lang.reflect.InvocationTargetException;
import java.io.IOException;
import java.util.Locale;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

/** Actual production request publication on the host filesystem and on ART/Windows. */
public final class ControlCenterRequestsTest {
    public static void main(String[] args) throws Exception {
        Method write = Class.forName("com.gesher.server.ControlCenter")
                .getDeclaredMethod("publishRequest", File.class, String.class, String.class);
        write.setAccessible(true);
        File directory = Files.createTempDirectory("gesher-panel-publish-").toFile();
        File requests = new File(directory, "control-panel-requests");
        Set<String> complete = new HashSet<>();
        String[] names = new String[8];
        for (int i = 0; i < names.length; i++) {
            names[i] = "com.example.writer" + i;
            for (int n = 0; n < 4; n++) {
                String token = String.format(Locale.ROOT, "%032x", i * 4 + n + 1);
                complete.add(names[i] + "\n" + token + "\n");
            }
        }
        CountDownLatch start = new CountDownLatch(1);
        AtomicBoolean done = new AtomicBoolean();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread observer = new Thread(() -> {
            try {
                start.await();
                while (!done.get()) {
                    File[] records = requests.listFiles((dir, name) -> name.endsWith(".request"));
                    if (records != null) for (File record : records) {
                        String value = new String(Files.readAllBytes(record.toPath()), StandardCharsets.UTF_8);
                        if (!complete.contains(value)) throw new AssertionError("Partial or mixed request: " + value);
                    }
                }
            } catch (Throwable t) { failure.compareAndSet(null, t); }
        }, "panel-request-observer");
        Thread[] writers = new Thread[names.length];
        try {
            observer.start();
            for (int i = 0; i < writers.length; i++) {
                final String name = names[i];
                final int writerIndex = i;
                writers[i] = new Thread(() -> {
                    try {
                        start.await();
                        for (int n = 0; n < 4; n++) {
                            String token = String.format(Locale.ROOT, "%032x", writerIndex * 4 + n + 1);
                            write.invoke(null, requests, name, token);
                        }
                    } catch (Throwable t) { failure.compareAndSet(null, t); }
                }, "panel-request-writer-" + i);
                writers[i].start();
            }
            start.countDown();
            for (Thread writer : writers) writer.join(30000);
            for (Thread writer : writers) if (writer.isAlive()) throw new AssertionError("Request writer did not finish");
            done.set(true);
            observer.join(30000);
            if (observer.isAlive()) throw new AssertionError("Request observer did not finish");
            if (failure.get() != null) throw new AssertionError("Concurrent publication failed", failure.get());
            File[] files = requests.listFiles();
            Set<String> actual = new HashSet<>();
            if (files != null) for (File record : files) {
                if (!record.getName().matches("[0-9]{19}-[0-9a-f]{32}\\.request")) {
                    throw new AssertionError("Request publication left an unexpected file: " + record);
                }
                actual.add(new String(Files.readAllBytes(record.toPath()), StandardCharsets.UTF_8));
            }
            if (files == null || files.length != 32 || !actual.equals(complete)) {
                throw new AssertionError("A concurrent writer lost or mixed a request");
            }
            try {
                write.invoke(null, requests, "com.example.extra", null);
                throw new AssertionError("An unbounded tokenless request was accepted");
            } catch (InvocationTargetException expected) {
                if (!(expected.getCause() instanceof IOException)) throw expected;
            }
            if (requests.list().length != 32) throw new AssertionError("Rejected publication modified the queue");
            System.out.println("ControlCenterRequestsTest all passed: 32 concurrent immutable requests, complete reads, bounded queue");
        } finally {
            start.countDown();
            done.set(true);
            for (Thread writer : writers) if (writer != null) writer.join(30000);
            observer.join(30000);
            File[] files = requests.listFiles();
            if (files != null) for (File file : files) file.delete();
            requests.delete();
            directory.delete();
        }
    }
}
