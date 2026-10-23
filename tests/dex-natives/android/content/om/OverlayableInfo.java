package android.content.om;

/** Stand-in for the framework class: what ApkAssets' natives construct. */
public final class OverlayableInfo {
  public final String name;
  public final String actor;

  public OverlayableInfo(String name, String actor) {
    this.name = name;
    this.actor = actor;
  }
}
