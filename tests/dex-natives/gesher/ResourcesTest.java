package gesher;

import android.content.res.ApkAssets;
import android.content.res.AssetManager;
import android.content.res.StringBlock;
import android.content.res.XmlBlock;
import android.util.TypedValue;
import java.nio.charset.StandardCharsets;

/**
 * libandroidfw through libandroid_runtime's asset natives (step 4 of
 * docs/framework-natives.md), on res.apk (tests/res-app, built by aapt2): an
 * APK loaded by ApkAssets, resources looked up and resolved by AssetManager,
 * a string from the APK's string pool, a theme's attributes, an asset read
 * from the zip, and the binary manifest walked by XmlBlock. The calls are the
 * ones the framework's Resources, Theme and XmlResourceParser make. Takes the
 * APK's path; prints "all passed" and exits 0 when every check holds.
 */
public final class ResourcesTest {
  private static final String PACKAGE = "gesher.res";
  // XmlPullParser's event types, as ResXMLParser returns them.
  private static final int START_TAG = 2;
  private static final int END_DOCUMENT = 1;

  private static int fails = 0;

  private static void check(boolean ok, String what) {
    if (!ok) {
      System.out.println("FAIL: " + what);
      fails++;
    }
  }

  public static void main(String[] args) throws Exception {
    System.loadLibrary("android_runtime");
    String path = args[0];

    long apk = ApkAssets.nativeLoad(ApkAssets.FORMAT_APK, path, 0, null);
    check(apk != 0, "ApkAssets.nativeLoad");
    check(path.equals(ApkAssets.nativeGetAssetPath(apk)), "nativeGetAssetPath");
    check(ApkAssets.nativeIsUpToDate(apk) != 0, "nativeIsUpToDate (critical)");
    ApkAssets[] assets = {new ApkAssets(apk)};

    long am = AssetManager.nativeCreate();
    check(am != 0, "AssetManager.nativeCreate");
    AssetManager.nativeSetApkAssets(am, assets, false, false);

    // Resource identifiers and names.
    int greeting = AssetManager.nativeGetResourceIdentifier(am, "greeting", "string", PACKAGE);
    int answer = AssetManager.nativeGetResourceIdentifier(am, "answer", "integer", PACKAGE);
    int theme = AssetManager.nativeGetResourceIdentifier(am, "GesherTheme", "style", PACKAGE);
    int color = AssetManager.nativeGetResourceIdentifier(am, "gesherColor", "attr", PACKAGE);
    int size = AssetManager.nativeGetResourceIdentifier(am, "gesherSize", "attr", PACKAGE);
    check(greeting == 0x7f030000, "id of string/greeting: 0x" + Integer.toHexString(greeting));
    check(answer != 0 && theme != 0 && color != 0 && size != 0, "ids of integer, style, attrs");
    check((PACKAGE + ":string/greeting").equals(AssetManager.nativeGetResourceName(am, greeting)),
        "nativeGetResourceName");
    check("string".equals(AssetManager.nativeGetResourceTypeName(am, greeting)),
        "nativeGetResourceTypeName");
    check(AssetManager.nativeGetResourceIdentifier(am, "missing", "string", PACKAGE) == 0,
        "unknown name has no id");

    // Values: an integer, and a string from the APK's string pool, as
    // AssetManager.getResourceText and getPooledStringForCookie read it.
    TypedValue value = new TypedValue();
    int cookie = AssetManager.nativeGetResourceValue(am, answer, (short) 0, value, true);
    check(cookie > 0, "nativeGetResourceValue(integer) cookie");
    check(value.type == TypedValue.TYPE_INT_DEC && value.data == 42, "integer/answer is 42");
    cookie = AssetManager.nativeGetResourceValue(am, greeting, (short) 0, value, true);
    check(cookie == 1 && value.type == TypedValue.TYPE_STRING, "string/greeting is a string");
    long pool = ApkAssets.nativeGetStringBlock(apk);
    String text = StringBlock.nativeGetString(pool, value.data);
    check("Hello from resources".equals(text), "the string's text: " + text);

    // A theme with GesherTheme applied resolves its attributes.
    long t = AssetManager.nativeThemeCreate(am);
    AssetManager.nativeThemeApplyStyle(am, t, theme, true);
    check(AssetManager.nativeThemeGetAttributeValue(am, t, color, value, true) > 0
            && value.type == TypedValue.TYPE_INT_COLOR_ARGB8 && value.data == 0xff112233,
        "theme attribute gesherColor = #ff112233");
    check(AssetManager.nativeThemeGetAttributeValue(am, t, size, value, true) > 0
            && value.data == 7, "theme attribute gesherSize = 7");
    int[] styleAttrs = AssetManager.nativeGetStyleAttributes(am, theme);
    check(styleAttrs != null && styleAttrs.length == 2, "nativeGetStyleAttributes");

    // An asset from the zip, through the AssetInputStream natives.
    long asset = AssetManager.nativeOpenAsset(am, "hello.txt", AssetManager.ACCESS_STREAMING);
    check(asset != 0, "nativeOpenAsset");
    byte[] buf = new byte[64];
    int n = AssetManager.nativeAssetRead(asset, buf, 0, buf.length);
    String assetText = new String(buf, 0, Math.max(n, 0), StandardCharsets.UTF_8);
    check("asset bytes from the APK\n".equals(assetText), "asset text: " + assetText);
    check(AssetManager.nativeAssetGetLength(asset) == n, "nativeAssetGetLength");
    AssetManager.nativeAssetDestroy(asset);
    String[] list = AssetManager.nativeList(am, "");
    check(list != null && java.util.Arrays.asList(list).contains("hello.txt"), "nativeList");

    // The binary manifest, as XmlResourceParser walks it.
    long xml = AssetManager.nativeOpenXmlAsset(am, 1, "AndroidManifest.xml");
    check(xml != 0, "nativeOpenXmlAsset");
    long xmlPool = XmlBlock.nativeGetStringBlock(xml);
    long parser = XmlBlock.nativeCreateParseState(xml, 0);
    StringBuilder tags = new StringBuilder();
    String packageName = null;
    int label = 0;
    for (int event = XmlBlock.nativeNext(parser); event != END_DOCUMENT && event >= 0;
        event = XmlBlock.nativeNext(parser)) {
      if (event != START_TAG) continue;
      String name = StringBlock.nativeGetString(xmlPool, XmlBlock.nativeGetName(parser));
      tags.append(name).append(' ');
      int count = XmlBlock.nativeGetAttributeCount(parser);
      for (int i = 0; i < count; i++) {
        String attr = StringBlock.nativeGetString(xmlPool,
            XmlBlock.nativeGetAttributeName(parser, i));
        if ("package".equals(attr)) {
          packageName = StringBlock.nativeGetString(xmlPool,
              XmlBlock.nativeGetAttributeStringValue(parser, i));
        } else if ("label".equals(attr)) {
          label = XmlBlock.nativeGetAttributeData(parser, i);
        }
      }
    }
    check("manifest application ".equals(tags.toString()), "manifest tags: " + tags);
    check(PACKAGE.equals(packageName), "manifest package: " + packageName);
    check(label == greeting, "application label references string/greeting");
    XmlBlock.nativeDestroyParseState(parser);
    XmlBlock.nativeDestroy(xml);

    AssetManager.nativeDestroy(am);
    ApkAssets.nativeDestroy(apk);

    System.out.println(fails == 0 ? "all passed" : fails + " failures");
    System.exit(fails == 0 ? 0 : 1);
  }
}
