package android.app;

public final class ActivityThread {
    public static Application application;

    private ActivityThread() {}

    public static Application currentApplication() {
        return application;
    }
}
