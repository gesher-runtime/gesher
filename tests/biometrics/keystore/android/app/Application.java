package android.app;

import java.io.File;

/** Test-only boundary for the software store's per-application directory. */
public final class Application {
    private final File dataDir;

    public Application(File dataDir) {
        this.dataDir = dataDir;
    }

    public File getDataDir() {
        return dataDir;
    }
}
