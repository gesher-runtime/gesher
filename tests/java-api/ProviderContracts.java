package gesher;

import dalvik.system.PathClassLoader;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

/** Exercises two real built-in library artifacts in a standalone ART process. */
public final class ProviderContracts {
    private static void check(boolean condition, String problem) {
        if (!condition) {
            throw new AssertionError(problem);
        }
    }

    private static void absent(String name, ClassLoader loader) throws Exception {
        try {
            Class.forName(name, false, loader);
        } catch (ClassNotFoundException expected) {
            return;
        }
        throw new AssertionError("unexpected class visibility: " + name);
    }

    private static Class<?> owned(String name, ClassLoader loader) throws Exception {
        Class<?> loaded = Class.forName(name, false, loader);
        check(loaded.getClassLoader() == loader, "wrong defining loader: " + name);
        return loaded;
    }

    public static void main(String[] args) throws Exception {
        check(args.length == 2, "expected android.test.base.jar and org.apache.http.legacy.jar");
        ClassLoader boot = Object.class.getClassLoader();
        ClassLoader junit = new PathClassLoader(args[0], boot);
        ClassLoader http = new PathClassLoader(args[1], boot);
        Class<?> bootBundle = owned("android.os.Bundle", boot);
        check(Class.forName(bootBundle.getName(), false, junit) == bootBundle, "JUnit path boot delegation");
        check(Class.forName(bootBundle.getName(), false, http) == bootBundle, "HTTP path boot delegation");

        String assertName = "junit.framework.Assert";
        String httpName = "org.apache.http.HttpVersion";
        absent(assertName, boot);
        absent(assertName, http);
        absent(httpName, boot);
        absent(httpName, junit);

        Class<?> assertClass = owned(assertName, junit);
        Method assertTrue = assertClass.getDeclaredMethod("assertTrue", boolean.class);
        assertTrue.invoke(null, true);
        Class<?> assertionFailed = owned("junit.framework.AssertionFailedError", junit);
        try {
            assertTrue.invoke(null, false);
            throw new AssertionError("JUnit assertTrue(false) returned");
        } catch (InvocationTargetException failure) {
            check(failure.getCause().getClass() == assertionFailed, "JUnit assertTrue(false) exception type");
        }
        System.out.println("PROVIDER android.test.base junit.framework.Assert child-owned and assertTrue executed");

        Class<?> versionClass = owned(httpName, http);
        Object version = versionClass.getField("HTTP_1_1").get(null);
        Method getMajor = versionClass.getMethod("getMajor");
        check(getMajor.getDeclaringClass() == owned("org.apache.http.ProtocolVersion", http),
                "HTTP getMajor defining class");
        check(Integer.valueOf(1).equals(getMajor.invoke(version)), "HTTP major version");
        check(Integer.valueOf(1).equals(versionClass.getMethod("getMinor").invoke(version)), "HTTP minor version");
        System.out.println("PROVIDER org.apache.http.legacy org.apache.http.HttpVersion child-owned and version read");
        System.out.println("ProviderContracts all passed");
    }
}
