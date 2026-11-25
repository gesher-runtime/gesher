/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Map;
import java.util.function.Consumer;
import java.util.function.Supplier;

/** One blocking native directory watch, with explicit overflow/invalidation recovery. */
final class DexoptProfileWatch implements Runnable {
    private final Path root;
    private final Supplier<Map<String, String>> packages;
    private final DexoptWorkQueue queue;
    private final Consumer<String> log;
    private final Thread thread;
    private volatile boolean stopped;
    private volatile long watch;

    DexoptProfileWatch(Path root, Supplier<Map<String, String>> packages,
            DexoptWorkQueue queue, Consumer<String> log) {
        this.root = root; this.packages = packages; this.queue = queue; this.log = log;
        thread = new Thread(this, "dexopt-profiles");
        thread.setDaemon(true);
    }

    void start() { thread.start(); }
    void refresh() {
        long active = watch;
        if (active != 0) DexoptProfileNotifications.wake(active);
        else thread.interrupt(); // registration failure backoff, before a watch exists
    }
    void stop() { stopped = true; refresh(); }

    @Override public void run() {
        long recovery = 1000;
        while (!stopped) {
            try {
                Files.createDirectories(root);
                // Native open queues recursive I/O before the installed snapshot.
                watch = DexoptProfileNotifications.open(Zygote.windowsPath(root.toString()));
                boolean refresh = true;
                while (!stopped) {
                    if (refresh) {
                        long registryEpoch = queue.registryEpoch();
                        Map<String, String> current = packages.get();
                        if (!queue.reconcile(current, registryEpoch)) continue;
                        queue.changedAll();
                        refresh = false;
                        recovery = 1000;
                    }
                    String[] changes = DexoptProfileNotifications.next(watch);
                    if (changes.length == 0) { refresh = true; continue; } // explicit wake or overflow
                    for (String changed : changes) {
                        String path = changed.replace('\\', '/');
                        int slash = path.indexOf('/');
                        if (slash < 0) { refresh = true; continue; } // package-directory membership
                        if (path.substring(slash + 1).equals("primary.prof"))
                            queue.changed(path.substring(0, slash));
                    }
                }
            } catch (IOException | RuntimeException failure) {
                if (!stopped) log.accept("dexopt: profile notifications unavailable; retry in "
                        + recovery + " ms: " + failure);
            } finally {
                long retired = watch;
                watch = 0;
                if (retired != 0) DexoptProfileNotifications.close(retired);
            }
            if (!stopped) {
                // This is failure recovery, never a clean-device package-scan timer.
                try { Thread.sleep(recovery); } catch (InterruptedException retryNow) { }
                recovery = Math.min(60_000, recovery * 2);
            }
        }
    }
}
