package android.util;

/** Stand-in for the framework class: the fields the asset natives fill in. */
public class TypedValue {
  public static final int TYPE_STRING = 0x03;
  public static final int TYPE_INT_DEC = 0x10;
  public static final int TYPE_INT_COLOR_ARGB8 = 0x1c;

  public int type;
  public int assetCookie;
  public int data;
  public int resourceId;
  public int changingConfigurations;
  public int density;
  public boolean usesFeatureFlags;

  // AssetManager's natives call it (gTypedValueOffsets.setFields).
  private void setFields(int type, int assetCookie, int data, int resourceId,
      int changingConfigurations, int density, boolean usesFeatureFlags) {
    this.type = type;
    this.assetCookie = assetCookie;
    this.data = data;
    this.resourceId = resourceId;
    this.changingConfigurations = changingConfigurations;
    this.density = density;
    this.usesFeatureFlags = usesFeatureFlags;
  }
}
