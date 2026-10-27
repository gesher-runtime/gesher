// The same process tests inside an APK attached to Gesher's real device.
package gesher;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

import java.io.File;

public final class ExecActivity extends Activity {
    private static native int nativeProcesses(String child);
    private static native int nativeEnvironment(String child);

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView view = new TextView(this);
        view.setText("Running exec3 process checks");
        setContentView(view);
        try {
            String directory = getApplicationInfo().nativeLibraryDir;
            String child = new File(directory, "libexec2child.so").getPath();
            System.loadLibrary("exec3jni");
            for (int i = 0; i < 16; i++) android.system.Os.setenv("GESHER_EXEC4_JAVA_GROW_" + i, "host", true);
            android.system.Os.setenv("GESHER_EXEC4_ENV", "java-host-setenv", true);
            int environmentResult = nativeEnvironment(child);
            if (environmentResult != 0) throw new AssertionError("live native environment suite failed: " + environmentResult);
            System.out.println("Exec4DeviceTest PASS host environment changes reach native environ and spawned children");
            if (nativeProcesses(child) != 0) throw new AssertionError("in-process native suite failed");
            System.out.println("Exec3DeviceTest PASS native fork/exec, spawn, popen, system, signals and waitpid in an app");
            Exec3Test.main(new String[0]);
            Exec3ProcessTest.main(new String[]{directory});
            Exec3PropertiesTest.main(new String[]{directory});
            Exec3LogTest.main(new String[]{directory});
            if (getIntent().getBooleanExtra("exec3.commands", false)) Exec3CommandsTest.main(new String[0]);
            view.setText("Exec3 process checks passed");
            System.out.println("Exec3DeviceTest PASS shell, Java process signals, tools and shared properties on the device");
        } catch (Throwable failure) {
            System.out.println("Exec3DeviceTest FAIL " + failure);
            failure.printStackTrace(System.out);
            view.setText("Exec3 process checks failed");
        }
        System.out.flush();
    }

    @Override public void onResume() {
        super.onResume();
        finish();
    }
}
