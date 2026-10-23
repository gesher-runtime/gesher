package android.util.proto;

/** Stand-in for the framework class: dumps to proto are not supported. */
public class ProtoOutputStream {
  public long start(long fieldId) {
    throw new UnsupportedOperationException("ProtoOutputStream");
  }

  public void end(long token) {}

  public void write(long fieldId, long val) {}

  public void write(long fieldId, int val) {}

  public void write(long fieldId, boolean val) {}

  public void write(long fieldId, String val) {}
}
