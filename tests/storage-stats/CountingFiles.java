/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.io.IOException;
import java.io.InterruptedIOException;
import java.nio.file.*;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** Counts real traversal calls/visits; gates and read faults are explicit test controls. */
final class CountingFiles {
    static final AtomicInteger walks = new AtomicInteger();
    static final AtomicInteger entries = new AtomicInteger();
    static volatile CountDownLatch gate;
    static volatile int failAt = -1;

    static void reset() {
        if (gate != null) throw new AssertionError("live filesystem gate");
        walks.set(0);
        entries.set(0);
        failAt = -1;
    }

    static void release() {
        CountDownLatch old = gate;
        gate = null;
        if (old != null) old.countDown();
    }

    static Path walkFileTree(Path root, Set<FileVisitOption> options, int depth,
            FileVisitor<? super Path> visitor) throws IOException {
        walks.incrementAndGet();
        CountDownLatch waiting = gate;
        if (waiting != null) {
            try {
                if (!waiting.await(20, TimeUnit.SECONDS)) throw new AssertionError("filesystem gate timeout");
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                throw new InterruptedIOException("fixture interrupted at filesystem boundary");
            }
        }
        return Files.walkFileTree(root, options, depth, new FileVisitor<Path>() {
            private void count(Path path) throws IOException {
                if (entries.incrementAndGet() == failAt) throw new AccessDeniedException(path.toString());
            }
            @Override public FileVisitResult preVisitDirectory(Path path, BasicFileAttributes a)
                    throws IOException { count(path); return visitor.preVisitDirectory(path, a); }
            @Override public FileVisitResult visitFile(Path path, BasicFileAttributes a)
                    throws IOException { count(path); return visitor.visitFile(path, a); }
            @Override public FileVisitResult visitFileFailed(Path path, IOException failure)
                    throws IOException { count(path); return visitor.visitFileFailed(path, failure); }
            @Override public FileVisitResult postVisitDirectory(Path path, IOException failure)
                    throws IOException { return visitor.postVisitDirectory(path, failure); }
        });
    }
}
