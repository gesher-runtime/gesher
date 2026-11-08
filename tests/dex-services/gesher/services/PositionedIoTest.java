/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.system.ErrnoException;
import android.system.Os;
import static android.system.OsConstants.*;

import java.io.FileDescriptor;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.Comparator;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

/** Real libcore JNI -> common POSIX adapter on Windows; no I/O mock. */
public final class PositionedIoTest {
    private static int checks;
    private static final byte[] DATA = new byte[]{'A', 'B', 'X', 0, 4, 5, 6, 7, 8, 9};
    private interface Call { void run() throws Exception; }
    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        ++checks;
    }
    private static void error(int expected, Call call, String message) throws Exception {
        try { call.run(); throw new AssertionError(message + ": accepted invalid operation"); }
        catch (ErrnoException actual) { require(actual.errno == expected, message + ": " + actual); }
    }
    private static FileDescriptor open(Path path, int mode) throws Exception {
        return Os.open(path.toString(), mode, 0600);
    }
    private static byte[] at(FileDescriptor fd, int length, long offset) throws Exception {
        byte[] bytes = new byte[length];
        require(Os.pread(fd, bytes, 0, length, offset) == length, "short positioned read at " + offset);
        return bytes;
    }
    private static void ordinary(Path directory) throws Exception {
        Path original = directory.resolve("original"), renamed = directory.resolve("renamed");
        FileDescriptor fd = open(original, O_CREAT | O_EXCL | O_RDWR);
        FileDescriptor alias = null;
        try {
            require(Os.write(fd, DATA, 0, DATA.length) == DATA.length, "initial ordinary write");
            require(Os.lseek(fd, 7, SEEK_SET) == 7, "set ordinary cursor");
            alias = Os.dup(fd);
            require(Arrays.equals(at(alias, 4, 0), Arrays.copyOf(DATA, 4)), "ABX header contents");
            require(Os.lseek(fd, 0, SEEK_CUR) == 7 && Os.lseek(alias, 0, SEEK_CUR) == 7,
                    "pread changed the shared cursor");
            byte[] next = new byte[1];
            require(Os.read(fd, next, 0, 1) == 1 && next[0] == 7, "ordinary read after header sniff");
            require(Os.pwrite(alias, new byte[]{42, 43}, 0, 2, 4) == 2, "positioned write");
            require(Os.lseek(fd, 0, SEEK_CUR) == 8 && Os.lseek(alias, 0, SEEK_CUR) == 8,
                    "pwrite changed the shared cursor");
            require(Arrays.equals(at(fd, 2, 4), new byte[]{42, 43}), "positioned write used its explicit offset");
            require(Os.pread(fd, next, 0, 1, 1L << 33) == 0 && Os.lseek(fd, 0, SEEK_CUR) == 8,
                    "64-bit EOF read changed cursor or truncated its offset");
            require(Os.pread(fd, next, 0, 0, 0) == 0 && Os.pwrite(fd, next, 0, 0, 500) == 0,
                    "zero-length positioned operation");
            require(Os.fstat(fd).st_size == DATA.length && Os.lseek(fd, 0, SEEK_CUR) == 8,
                    "zero-length operation extended the file or changed cursor");
            error(EINVAL, () -> Os.pread(fd, next, 0, 1, -1), "negative pread offset");
            error(EINVAL, () -> Os.pwrite(fd, next, 0, 1, -1), "negative pwrite offset");
            FileDescriptor ro = open(original, O_RDONLY), wo = open(original, O_WRONLY);
            try {
                error(EBADF, () -> Os.pwrite(ro, next, 0, 1, 0), "read-only descriptor gained write access");
                error(EBADF, () -> Os.pread(wo, next, 0, 1, 0), "write-only descriptor gained read access");
                require(Os.lseek(ro, 0, SEEK_CUR) == 0 && Os.lseek(wo, 0, SEEK_CUR) == 0,
                        "failed access check changed cursor");
            } finally { Os.close(ro); Os.close(wo); }
            Os.rename(original.toString(), renamed.toString());
            Files.write(original, new byte[]{99});
            require(at(fd, 1, 0)[0] == 'A', "renamed descriptor resolved a replacement pathname");
            Os.unlink(renamed.toString());
            require(at(fd, 1, 0)[0] == 'A', "unlinked open descriptor lost its contents");
            require(Os.pwrite(fd, new byte[]{44}, 0, 1, 0) == 1 && at(alias, 1, 0)[0] == 44,
                    "unlinked open descriptor cannot receive positioned writes");
            require(Os.lseek(fd, 0, SEEK_CUR) == 8 && Arrays.equals(Files.readAllBytes(original), new byte[]{99}),
                    "rename/unlink positioned I/O changed shared cursor or replacement file");
        } finally { if (alias != null) Os.close(alias); Os.close(fd); }
        FileDescriptor dir = open(directory, O_RDONLY);
        try { error(EISDIR, () -> Os.pread(dir, new byte[1], 0, 1, 0), "directory read"); }
        finally { Os.close(dir); }
        FileDescriptor[] pipe = Os.pipe();
        try {
            require(Os.write(pipe[1], new byte[]{23}, 0, 1) == 1, "pipe witness write");
            error(ESPIPE, () -> Os.pread(pipe[0], new byte[1], 0, 1, 0), "pread consumed a pipe");
            error(ESPIPE, () -> Os.pwrite(pipe[1], new byte[]{24}, 0, 1, 0), "pwrite wrote to a pipe");
            byte[] actual = new byte[1];
            require(Os.read(pipe[0], actual, 0, 1) == 1 && actual[0] == 23, "pipe error consumed existing input");
        } finally { Os.close(pipe[0]); Os.close(pipe[1]); }
        FileDescriptor memfd = Os.memfd_create("positioned-delete-on-close", 0);
        try {
            require(Os.write(memfd, DATA, 0, DATA.length) == DATA.length && Os.lseek(memfd, 3, SEEK_SET) == 3,
                    "memfd fixture");
            require(Arrays.equals(at(memfd, 4, 0), Arrays.copyOf(DATA, 4)), "delete-on-close read");
            require(Os.pwrite(memfd, new byte[]{45}, 0, 1, 1) == 1 && at(memfd, 1, 1)[0] == 45,
                    "delete-on-close write");
            require(Os.lseek(memfd, 0, SEEK_CUR) == 3, "delete-on-close I/O changed cursor");
        } finally { Os.close(memfd); }
    }
    private static void concurrent(Path directory) throws Exception {
        FileDescriptor fd = open(directory.resolve("concurrent"), O_CREAT | O_EXCL | O_RDWR);
        FileDescriptor alias = null;
        try {
            byte[] data = new byte[4096];
            for (int i = 0; i < data.length; ++i) data[i] = (byte)(i % 251);
            require(Os.write(fd, data, 0, data.length) == data.length && Os.lseek(fd, 0, SEEK_SET) == 0,
                    "concurrent fixture");
            alias = Os.dup(fd);
            final FileDescriptor peer = alias;
            AtomicReference<Throwable> failure = new AtomicReference<>();
            CountDownLatch start = new CountDownLatch(1);
            Thread positioned = new Thread(() -> {
                try {
                    start.await(); byte[] byteAt = new byte[1];
                    for (int i = 0; i < 512; ++i) {
                        if (Os.pread(peer, byteAt, 0, 1, 3000) != 1 || byteAt[0] != data[3000])
                            throw new AssertionError("concurrent pread returned wrong contents");
                        if (Os.pwrite(peer, new byte[]{67}, 0, 1, 3500) != 1)
                            throw new AssertionError("concurrent pwrite failed");
                    }
                } catch (Throwable problem) { failure.set(problem); }
            }, "positioned-io-contract");
            positioned.setDaemon(true); positioned.start(); start.countDown();
            try {
                byte[] actual = new byte[1];
                for (int i = 0; i < 512; ++i) {
                    if (Os.read(fd, actual, 0, 1) != 1 || actual[0] != data[i])
                        throw new AssertionError("positioned I/O raced ordinary read at " + i);
                    Thread.yield();
                }
            } finally { positioned.join(30000); }
            require(!positioned.isAlive(), "positioned I/O did not finish");
            if (failure.get() != null) throw new AssertionError("positioned worker failed", failure.get());
            require(Os.lseek(fd, 0, SEEK_CUR) == 512 && Os.lseek(peer, 0, SEEK_CUR) == 512,
                    "concurrent positioned I/O changed the ordinary cursor");
            require(at(fd, 1, 3500)[0] == 67, "concurrent positioned write did not persist");
        } finally { if (alias != null) Os.close(alias); Os.close(fd); }
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        Path parent = Path.of(System.getenv("ANDROID_DATA")); Files.createDirectories(parent);
        Path directory = Files.createTempDirectory(parent, "positioned-io-");
        try { ordinary(directory); concurrent(directory); }
        finally {
            try (var files = Files.walk(directory)) {
                for (Path path : files.sorted(Comparator.reverseOrder()).toList()) Files.deleteIfExists(path);
            }
        }
        System.out.println("PASS PositionedIoTest (" + checks + " assertions)");
    }
}
