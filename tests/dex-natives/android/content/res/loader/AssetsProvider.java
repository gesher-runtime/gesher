package android.content.res.loader;

import android.content.res.AssetFileDescriptor;

/** Stand-in for the framework interface. */
public interface AssetsProvider {
  default AssetFileDescriptor loadAssetFd(String path, int accessMode) {
    return null;
  }
}
