/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
import java.nio.file.Path;
public final class WindowsWifiHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Path expected = Path.of(args[1]).toRealPath();
        for (String name : new String[]{"com.gesher.net.WindowsWifi", "com.gesher.net.WindowsWifiFixture"}) {
            Path actual = Path.of(Class.forName(name).getProtectionDomain().getCodeSource().getLocation().toURI()).toRealPath();
            if (!actual.equals(expected)) throw new AssertionError("Wrong provider: " + name);
        }
        com.gesher.net.WindowsWifiFixture.run(true);
    }
}
