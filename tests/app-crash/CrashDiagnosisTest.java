/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import com.gesher.app.CrashDiagnosis;
import com.gesher.app.AppCrash;
import com.gesher.app.window.WindowSession;
import com.gesher.server.WindowsBluetooth;
import com.gesher.server.Zygote;

public final class CrashDiagnosisTest {
    private static int checks;
    private static void check(boolean value) {
        checks++;
        if (!value) throw new AssertionError("check " + checks);
    }
    public static void main(String[] args) throws Exception {
        if (args.length != 0) {
            WindowSession.shown = true;
            AppCrash.report(new IllegalStateException());
            check(Zygote.command == null);
            System.out.println("PASS: explicit dialog suppression");
            return;
        }
        Throwable npe = new NullPointerException("Attempt to invoke virtual method "
                + "'android.bluetooth.le.BluetoothLeScanner android.bluetooth.BluetoothAdapter."
                + "getBluetoothLeScanner()' on a null object reference");
        check(CrashDiagnosis.kind(npe, () -> false).equals("bluetooth"));
        check(CrashDiagnosis.kind(npe, () -> true).equals("generic"));
        check(CrashDiagnosis.kind(npe, () -> { throw new UnsatisfiedLinkError(); }).equals("generic"));
        check(CrashDiagnosis.kind(new RuntimeException(npe), () -> false).equals("bluetooth"));
        check(CrashDiagnosis.kind(new NullPointerException("BluetoothAdapter"), () -> false).equals("generic"));
        java.util.concurrent.atomic.AtomicInteger probes = new java.util.concurrent.atomic.AtomicInteger();
        check(CrashDiagnosis.kind(new NullPointerException("Attempt to invoke virtual method "
                + "'void android.hardware.usb.UsbManager.open()' on a null object reference"),
                () -> { probes.incrementAndGet(); return false; }).equals("generic"));
        check(probes.get() == 0);
        check(CrashDiagnosis.kind(new SecurityException("denied"), () -> true).equals("security"));
        check(CrashDiagnosis.kind(new RuntimeException(new NoSuchMethodError()), () -> true).equals("compatibility"));
        check(CrashDiagnosis.kind(new UnsatisfiedLinkError(), () -> true).equals("compatibility"));
        check(CrashDiagnosis.kind(new ExceptionInInitializerError(new NullPointerException()), () -> true).equals("generic"));
        check(CrashDiagnosis.kind(new BootstrapMethodError(new IllegalStateException()), () -> true).equals("generic"));
        check(CrashDiagnosis.kind(new IllegalStateException(), () -> true).equals("generic"));
        Throwable cycle = new Throwable() { @Override public Throwable getCause() { return this; } };
        check(CrashDiagnosis.kind(cycle, () -> true).equals("generic"));

        // Run the complete production reporting method. Only Android/Windows boundaries
        // are stand-ins; no real hardware, process or UI is claimed by this host test.
        WindowSession.shown = false;
        AppCrash.report(npe);
        check(Zygote.command == null);
        WindowSession.shown = true;
        WindowsBluetooth.hr = 0x8007048f;
        AppCrash.report(npe);
        check(Zygote.command.contains(" --app-crash "));
        check(Zygote.command.contains("\"bluetooth\""));
        check(Zygote.command.contains("test.pkg"));
        check(Zygote.breakaway);
        WindowsBluetooth.hr = 0x80004001; // missing JNI is not absent hardware
        AppCrash.report(npe);
        check(Zygote.command.contains("\"generic\""));
        WindowsBluetooth.hr = 0;
        WindowsBluetooth.present = true;
        AppCrash.report(npe);
        check(Zygote.command.contains("\"generic\""));
        Zygote.fail = true;
        AppCrash.report(npe); // a failed notice must not replace the app's failure
        check(npe.getMessage().endsWith("on a null object reference"));

        java.io.File report = new java.io.File("logs/events.crash.log");
        String first = com.gesher.app.CrashReport.append(report, "test.pkg", 1,
                new IllegalStateException("FIRST-EVENT"));
        String second = com.gesher.app.CrashReport.append(report, "test.pkg", 2,
                new IllegalStateException("SECOND-EVENT"));
        String evidence = java.nio.file.Files.readString(report.toPath());
        check(!first.equals(second));
        check(evidence.contains(first) && evidence.contains("FIRST-EVENT"));
        check(evidence.contains(second) && evidence.contains("SECOND-EVENT"));
        String longMessage = "x".repeat(200_000);
        com.gesher.app.CrashReport.append(report, "test.pkg", 3, new Exception(longMessage));
        evidence = java.nio.file.Files.readString(report.toPath());
        check(evidence.length() < 140_000 && evidence.contains("[stack trace truncated]"));
        System.out.println("PASS: " + checks + " crash diagnosis/reporting host checks");
    }
}
