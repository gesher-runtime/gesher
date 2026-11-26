/* SPDX-License-Identifier: Apache-2.0 */
import java.io.BufferedReader;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;

public final class BootClassCensus {
    public static void main(String[] args) throws Exception {
        int count = 0;
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                new FileInputStream(args[0]), StandardCharsets.UTF_8))) {
            for (String name; (name = reader.readLine()) != null; ) {
                String outcome;
                try {
                    Class.forName(name, false, null);
                    outcome = "resolved";
                } catch (ClassNotFoundException missing) {
                    outcome = "absent";
                } catch (LinkageError failed) {
                    outcome = "linkage:" + failed.getClass().getSimpleName();
                } catch (SecurityException restricted) {
                    outcome = "restricted";
                }
                System.out.println("BOOT_CLASS\t" + outcome + "\t" + name);
                count++;
            }
        }
        System.out.println("BOOT_TOTAL\t" + count);
    }
}
