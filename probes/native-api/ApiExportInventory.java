package gesher;

/** Presence only. Does not invoke resolved API pointers or start an app/device. */
public final class ApiExportInventory {
  private static native int scan(String input, String output);

  public static void main(String[] args) throws InterruptedException {
    if (args.length != 3 && args.length != 4) throw new IllegalArgumentException("probe, manifest, output, optional hold ms required");
    System.load(args[0]);
    int checked = scan(args[1], args[2]);
    if (checked < 0) throw new AssertionError("Native inventory failed: " + checked);
    System.out.println("Native API inventory complete: " + checked);
    // Optional short hold lets the parent read only this fixture's loaded
    // translator module path. No debugger or thread suspension is involved.
    if (args.length == 4) Thread.sleep(Integer.parseInt(args[3]));
  }
}
