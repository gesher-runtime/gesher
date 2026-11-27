/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
import com.gesher.server.Device;
import com.gesher.server.GesherService;
import com.gesher.server.Zygote;
import java.util.Objects;
import java.util.Properties;

/** Host-shell-only control of the two synthetic fixture packages on a fresh device. */
public final class WindowPromotionControl {
    public static void main(String[] args) throws Exception {
        android.system.Os.setenv("GESHER_BINDER_UID", "2000", true);
        System.loadLibrary("android_runtime");
        if (args.length != 1) throw new IllegalArgumentException("explicit-headless");
        String target = "com.gesher.windowpromotion.target";
        if (args[0].equals("explicit-headless")) {
            Properties state = Device.read(Device.stateFile());
            if (state == null || !Objects.equals(System.getenv("GESHER_BINDER"),
                    state.getProperty(Device.STATE_DEVICE))) {
                throw new IllegalStateException("No matching fixture device");
            }
            if ("true".equals(state.getProperty(Device.STATE_PROTECTED))
                    && !Zygote.nativePrepareProtectedOperator(Device.pid(state, Device.BINDERD),
                            Device.startTime(state, Device.BINDERD))) {
                throw new SecurityException("Could not verify protected fixture operator");
            }
            GesherService.Client control = GesherService.Client.find();
            if (control == null) throw new IllegalStateException("No fixture device");
            control.setWindowMode(target, "headless");
            System.out.println("promotion: explicit-headless policy set");
        } else throw new IllegalArgumentException("Unknown fixture command");
    }
}
