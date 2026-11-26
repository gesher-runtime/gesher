import android.os.IBinder;
import android.os.ServiceManager;
import java.util.Arrays;

public final class RegistryProbe {
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        String[] names = ServiceManager.listServices();
        if (names == null || names.length == 0) {
            throw new IllegalStateException("No services returned by the live manager");
        }
        Arrays.sort(names);
        int missing = 0;
        for (String name : names) {
            IBinder binder = ServiceManager.checkService(name);
            if (binder == null) {
                ++missing;
                System.out.println("A03 SERVICE " + name + " <missing>");
            } else {
                System.out.println("A03 SERVICE " + name + " " + binder.getInterfaceDescriptor());
            }
        }
        System.out.println("A03 COMPLETE published=" + names.length + " missing=" + missing);
    }
}
