package dalvik.system;
public final class VMRuntime {
    public static VMRuntime getRuntime() { return new VMRuntime(); }
    public String bootClassPath() { return "boot.jar"; }
}
