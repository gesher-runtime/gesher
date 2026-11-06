package gesher;

/** Runs only in a disposable VM: exercise ART's Windows fatal signal report. */
public final class FatalContext {
  public static void main(String[] args) {
    if (args.length != 2 || !(args[1].equals("native") || args[1].equals("host"))) {
      throw new IllegalArgumentException("library path and native/host required");
    }
    System.load(args[0]);
    System.err.println("FatalContext entering " + args[1]);
    System.err.flush();
    JniTest.fatalNullRead(args[1].equals("host"));
    throw new AssertionError("Native null read returned");
  }
}
