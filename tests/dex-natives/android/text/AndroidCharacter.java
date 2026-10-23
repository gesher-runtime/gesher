package android.text;

/** Stand-in for the framework class; the natives match android_text_AndroidCharacter.cpp. */
public class AndroidCharacter {
  public static native void getDirectionalities(char[] src, byte[] dest, int count);

  public static native int getEastAsianWidth(char input);

  public static native void getEastAsianWidths(char[] src, int start, int count, byte[] dest);

  public static native boolean mirror(char[] text, int start, int count);

  public static native char getMirror(char ch);
}
