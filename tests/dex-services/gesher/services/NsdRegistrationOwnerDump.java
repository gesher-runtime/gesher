/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.os.IBinder;
import android.os.ServiceManager;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

/** Host-shell read-only observation of the real remote registration owner. */
public final class NsdRegistrationOwnerDump {
    public static void main(String[] args) {
        try {
            if (args.length != 1 || System.getenv("GESHER_BINDER") == null) {
                throw new IllegalArgumentException("output-file; existing Binder device required");
            }
            // Same genuine control-plane identity as RuntimeCommands. This process
            // never submits registration calls or claims an application's ProcessRecord.
            android.system.Os.setenv("GESHER_BINDER_UID", "2000", true);
            System.loadLibrary("android_runtime");
            IBinder owner = ServiceManager.checkService("gesher.nsd.registration");
            if (owner == null || !owner.getClass().getName().equals("android.os.BinderProxy")) {
                throw new AssertionError("registration owner is not a real remote Binder provider");
            }
            Path output = Path.of(args[0]);
            try (FileOutputStream stream = new FileOutputStream(output.toFile())) {
                owner.dump(stream.getFD(), new String[0]);
            }
            String text = Files.readString(output, StandardCharsets.UTF_8);
            if (!text.matches("(?s).*NSD_REGISTRATION_STATE pid=\\d+ clients=\\d+ requests=\\d+"
                    + " native_requests=\\d+ sockets=\\d+ watches=\\d+ callbacks=\\d+"
                    + " polled_requests=\\d+ sent_datagrams=\\d+ pending_withdrawals=\\d+.*")) {
                throw new AssertionError("missing or unavailable real owner counters: " + text);
            }
            System.out.print(text); System.out.flush();
        } catch (Throwable error) {
            error.printStackTrace(System.err); System.err.flush(); System.exit(1);
        }
    }
}
