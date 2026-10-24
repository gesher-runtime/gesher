package com.gesher.app.webview;

import android.webkit.WebSettings;

/**
 * WebSettings of a Gesher WebView (docs/webview.md, "Settings"): every value is kept, with
 * Android WebView's defaults; the ones WebView2 has an equivalent for reach it through
 * the provider (GesherWebViewProvider.applySettings) whenever one changes. Generated from
 * WebSettings' abstract methods, then edited by hand.
 */
final class GesherWebSettings extends WebSettings {
    private final Runnable mChanged;
    private String mUserAgentString;
    private final String mDefaultUserAgent;

    GesherWebSettings(String defaultUserAgent, Runnable changed) {
        mDefaultUserAgent = defaultUserAgent;
        mChanged = changed;
    }

    private void changed() {
        if (mChanged != null) mChanged.run();
    }

    @Override
    public synchronized void setUserAgentString(String ua) {
        mUserAgentString = ua == null || ua.isEmpty() ? null : ua;
        changed();
    }

    @Override
    public synchronized String getUserAgentString() {
        return mUserAgentString != null ? mUserAgentString : mDefaultUserAgent;
    }
    private boolean mNavDump = false;
    private boolean mSupportZoom = true;
    private boolean mMediaPlaybackRequiresUserGesture = true;
    private boolean mBuiltInZoomControls = false;
    private boolean mDisplayZoomControls = true;
    private boolean mAllowFileAccess = false;
    private boolean mAllowContentAccess = true;
    private boolean mLoadWithOverviewMode = false;
    private boolean mEnableSmoothTransition = false;
    private boolean mUseWebViewBackgroundForOverscrollBackground = false;
    private boolean mSaveFormData = true;
    private boolean mSavePassword = false;
    private int mTextZoom = 100;
    private boolean mAcceptThirdPartyCookies = false;
    private ZoomDensity mDefaultZoom = ZoomDensity.MEDIUM;
    private boolean mLightTouchEnabled = false;
    private int mUserAgent = 0;
    private boolean mUseWideViewPort = false;
    private boolean mSupportMultipleWindows = false;
    private LayoutAlgorithm mLayoutAlgorithm = LayoutAlgorithm.NARROW_COLUMNS;
    private String mStandardFontFamily = "sans-serif";
    private String mFixedFontFamily = "monospace";
    private String mSansSerifFontFamily = "sans-serif";
    private String mSerifFontFamily = "serif";
    private String mCursiveFontFamily = "cursive";
    private String mFantasyFontFamily = "fantasy";
    private int mMinimumFontSize = 8;
    private int mMinimumLogicalFontSize = 8;
    private int mDefaultFontSize = 16;
    private int mDefaultFixedFontSize = 13;
    private boolean mLoadsImagesAutomatically = true;
    private boolean mBlockNetworkImage = false;
    private boolean mBlockNetworkLoads = false;
    private boolean mJavaScriptEnabled = false;
    private boolean mAllowUniversalAccessFromFileURLs = false;
    private boolean mAllowFileAccessFromFileURLs = false;
    private boolean mPluginsEnabled = false;
    private PluginState mPluginState = PluginState.OFF;
    private String mDatabasePath = "";
    private String mGeolocationDatabasePath = null;
    private boolean mDatabaseEnabled = false;
    private boolean mDomStorageEnabled = false;
    private boolean mGeolocationEnabled = true;
    private boolean mJavaScriptCanOpenWindowsAutomatically = false;
    private String mDefaultTextEncodingName = "UTF-8";
    private boolean mNeedInitialFocus = true;
    private RenderPriority mRenderPriority = null;
    private int mCacheMode = WebSettings.LOAD_DEFAULT;
    private int mMixedContentMode = WebSettings.MIXED_CONTENT_NEVER_ALLOW;
    private boolean mVideoOverlayForEmbeddedEncryptedVideoEnabled = false;
    private boolean mOffscreenPreRaster = false;
    private boolean mSafeBrowsingEnabled = true;
    private int mDisabledActionModeMenuItems = 0;

    @Override
    public synchronized void setNavDump(boolean enabled) {
        mNavDump = enabled;
        changed();
    }

    @Override
    public synchronized boolean getNavDump() {
        return mNavDump;
    }

    @Override
    public synchronized void setSupportZoom(boolean support) {
        mSupportZoom = support;
        changed();
    }

    @Override
    public synchronized boolean supportZoom() {
        return mSupportZoom;
    }

    @Override
    public synchronized void setMediaPlaybackRequiresUserGesture(boolean require) {
        mMediaPlaybackRequiresUserGesture = require;
        changed();
    }

    @Override
    public synchronized boolean getMediaPlaybackRequiresUserGesture() {
        return mMediaPlaybackRequiresUserGesture;
    }

    @Override
    public synchronized void setBuiltInZoomControls(boolean enabled) {
        mBuiltInZoomControls = enabled;
        changed();
    }

    @Override
    public synchronized boolean getBuiltInZoomControls() {
        return mBuiltInZoomControls;
    }

    @Override
    public synchronized void setDisplayZoomControls(boolean enabled) {
        mDisplayZoomControls = enabled;
        changed();
    }

    @Override
    public synchronized boolean getDisplayZoomControls() {
        return mDisplayZoomControls;
    }

    @Override
    public synchronized void setAllowFileAccess(boolean allow) {
        mAllowFileAccess = allow;
        changed();
    }

    @Override
    public synchronized boolean getAllowFileAccess() {
        return mAllowFileAccess;
    }

    @Override
    public synchronized void setAllowContentAccess(boolean allow) {
        mAllowContentAccess = allow;
        changed();
    }

    @Override
    public synchronized boolean getAllowContentAccess() {
        return mAllowContentAccess;
    }

    @Override
    public synchronized void setLoadWithOverviewMode(boolean overview) {
        mLoadWithOverviewMode = overview;
        changed();
    }

    @Override
    public synchronized boolean getLoadWithOverviewMode() {
        return mLoadWithOverviewMode;
    }

    @Override
    public synchronized void setEnableSmoothTransition(boolean enable) {
        mEnableSmoothTransition = enable;
        changed();
    }

    @Override
    public synchronized boolean enableSmoothTransition() {
        return mEnableSmoothTransition;
    }

    @Override
    public synchronized void setUseWebViewBackgroundForOverscrollBackground(boolean view) {
        mUseWebViewBackgroundForOverscrollBackground = view;
        changed();
    }

    @Override
    public synchronized boolean getUseWebViewBackgroundForOverscrollBackground() {
        return mUseWebViewBackgroundForOverscrollBackground;
    }

    @Override
    public synchronized void setSaveFormData(boolean save) {
        mSaveFormData = save;
        changed();
    }

    @Override
    public synchronized boolean getSaveFormData() {
        return mSaveFormData;
    }

    @Override
    public synchronized void setSavePassword(boolean save) {
        mSavePassword = save;
        changed();
    }

    @Override
    public synchronized boolean getSavePassword() {
        return mSavePassword;
    }

    @Override
    public synchronized void setTextZoom(int textZoom) {
        mTextZoom = textZoom;
        changed();
    }

    @Override
    public synchronized int getTextZoom() {
        return mTextZoom;
    }

    @Override
    public synchronized void setAcceptThirdPartyCookies(boolean accept) {
        mAcceptThirdPartyCookies = accept;
        changed();
    }

    @Override
    public synchronized boolean getAcceptThirdPartyCookies() {
        return mAcceptThirdPartyCookies;
    }

    @Override
    public synchronized void setDefaultZoom(ZoomDensity zoom) {
        mDefaultZoom = zoom;
        changed();
    }

    @Override
    public synchronized ZoomDensity getDefaultZoom() {
        return mDefaultZoom;
    }

    @Override
    public synchronized void setLightTouchEnabled(boolean enabled) {
        mLightTouchEnabled = enabled;
        changed();
    }

    @Override
    public synchronized boolean getLightTouchEnabled() {
        return mLightTouchEnabled;
    }

    @Override
    public synchronized void setUserAgent(int ua) {
        mUserAgent = ua;
        changed();
    }

    @Override
    public synchronized int getUserAgent() {
        return mUserAgent;
    }

    @Override
    public synchronized void setUseWideViewPort(boolean use) {
        mUseWideViewPort = use;
        changed();
    }

    @Override
    public synchronized boolean getUseWideViewPort() {
        return mUseWideViewPort;
    }

    @Override
    public synchronized void setSupportMultipleWindows(boolean support) {
        mSupportMultipleWindows = support;
        changed();
    }

    @Override
    public synchronized boolean supportMultipleWindows() {
        return mSupportMultipleWindows;
    }

    @Override
    public synchronized void setLayoutAlgorithm(LayoutAlgorithm l) {
        mLayoutAlgorithm = l;
        changed();
    }

    @Override
    public synchronized LayoutAlgorithm getLayoutAlgorithm() {
        return mLayoutAlgorithm;
    }

    @Override
    public synchronized void setStandardFontFamily(String font) {
        mStandardFontFamily = font;
        changed();
    }

    @Override
    public synchronized String getStandardFontFamily() {
        return mStandardFontFamily;
    }

    @Override
    public synchronized void setFixedFontFamily(String font) {
        mFixedFontFamily = font;
        changed();
    }

    @Override
    public synchronized String getFixedFontFamily() {
        return mFixedFontFamily;
    }

    @Override
    public synchronized void setSansSerifFontFamily(String font) {
        mSansSerifFontFamily = font;
        changed();
    }

    @Override
    public synchronized String getSansSerifFontFamily() {
        return mSansSerifFontFamily;
    }

    @Override
    public synchronized void setSerifFontFamily(String font) {
        mSerifFontFamily = font;
        changed();
    }

    @Override
    public synchronized String getSerifFontFamily() {
        return mSerifFontFamily;
    }

    @Override
    public synchronized void setCursiveFontFamily(String font) {
        mCursiveFontFamily = font;
        changed();
    }

    @Override
    public synchronized String getCursiveFontFamily() {
        return mCursiveFontFamily;
    }

    @Override
    public synchronized void setFantasyFontFamily(String font) {
        mFantasyFontFamily = font;
        changed();
    }

    @Override
    public synchronized String getFantasyFontFamily() {
        return mFantasyFontFamily;
    }

    @Override
    public synchronized void setMinimumFontSize(int size) {
        mMinimumFontSize = size;
        changed();
    }

    @Override
    public synchronized int getMinimumFontSize() {
        return mMinimumFontSize;
    }

    @Override
    public synchronized void setMinimumLogicalFontSize(int size) {
        mMinimumLogicalFontSize = size;
        changed();
    }

    @Override
    public synchronized int getMinimumLogicalFontSize() {
        return mMinimumLogicalFontSize;
    }

    @Override
    public synchronized void setDefaultFontSize(int size) {
        mDefaultFontSize = size;
        changed();
    }

    @Override
    public synchronized int getDefaultFontSize() {
        return mDefaultFontSize;
    }

    @Override
    public synchronized void setDefaultFixedFontSize(int size) {
        mDefaultFixedFontSize = size;
        changed();
    }

    @Override
    public synchronized int getDefaultFixedFontSize() {
        return mDefaultFixedFontSize;
    }

    @Override
    public synchronized void setLoadsImagesAutomatically(boolean flag) {
        mLoadsImagesAutomatically = flag;
        changed();
    }

    @Override
    public synchronized boolean getLoadsImagesAutomatically() {
        return mLoadsImagesAutomatically;
    }

    @Override
    public synchronized void setBlockNetworkImage(boolean flag) {
        mBlockNetworkImage = flag;
        changed();
    }

    @Override
    public synchronized boolean getBlockNetworkImage() {
        return mBlockNetworkImage;
    }

    @Override
    public synchronized void setBlockNetworkLoads(boolean flag) {
        mBlockNetworkLoads = flag;
        changed();
    }

    @Override
    public synchronized boolean getBlockNetworkLoads() {
        return mBlockNetworkLoads;
    }

    @Override
    public synchronized void setJavaScriptEnabled(boolean flag) {
        mJavaScriptEnabled = flag;
        changed();
    }

    @Override
    public synchronized void setAllowUniversalAccessFromFileURLs(boolean flag) {
        mAllowUniversalAccessFromFileURLs = flag;
        changed();
    }

    @Override
    public synchronized void setAllowFileAccessFromFileURLs(boolean flag) {
        mAllowFileAccessFromFileURLs = flag;
        changed();
    }

    @Override
    public synchronized void setPluginsEnabled(boolean flag) {
        mPluginsEnabled = flag;
        changed();
    }

    @Override
    public synchronized void setPluginState(PluginState state) {
        mPluginState = state;
        changed();
    }

    @Override
    public synchronized void setDatabasePath(String databasePath) {
        mDatabasePath = databasePath;
        changed();
    }

    @Override
    public synchronized void setGeolocationDatabasePath(String databasePath) {
        mGeolocationDatabasePath = databasePath;
        changed();
    }

    @Override
    public synchronized void setDatabaseEnabled(boolean flag) {
        mDatabaseEnabled = flag;
        changed();
    }

    @Override
    public synchronized void setDomStorageEnabled(boolean flag) {
        mDomStorageEnabled = flag;
        changed();
    }

    @Override
    public synchronized boolean getDomStorageEnabled() {
        return mDomStorageEnabled;
    }

    @Override
    public synchronized String getDatabasePath() {
        return mDatabasePath;
    }

    @Override
    public synchronized boolean getDatabaseEnabled() {
        return mDatabaseEnabled;
    }

    @Override
    public synchronized void setGeolocationEnabled(boolean flag) {
        mGeolocationEnabled = flag;
        changed();
    }

    @Override
    public synchronized boolean getJavaScriptEnabled() {
        return mJavaScriptEnabled;
    }

    @Override
    public synchronized boolean getAllowUniversalAccessFromFileURLs() {
        return mAllowUniversalAccessFromFileURLs;
    }

    @Override
    public synchronized boolean getAllowFileAccessFromFileURLs() {
        return mAllowFileAccessFromFileURLs;
    }

    @Override
    public synchronized boolean getPluginsEnabled() {
        return mPluginsEnabled;
    }

    @Override
    public synchronized PluginState getPluginState() {
        return mPluginState;
    }

    @Override
    public synchronized void setJavaScriptCanOpenWindowsAutomatically(boolean flag) {
        mJavaScriptCanOpenWindowsAutomatically = flag;
        changed();
    }

    @Override
    public synchronized boolean getJavaScriptCanOpenWindowsAutomatically() {
        return mJavaScriptCanOpenWindowsAutomatically;
    }

    @Override
    public synchronized void setDefaultTextEncodingName(String encoding) {
        mDefaultTextEncodingName = encoding;
        changed();
    }

    @Override
    public synchronized String getDefaultTextEncodingName() {
        return mDefaultTextEncodingName;
    }

    @Override
    public synchronized void setNeedInitialFocus(boolean flag) {
        mNeedInitialFocus = flag;
        changed();
    }

    @Override
    public synchronized void setRenderPriority(RenderPriority priority) {
        mRenderPriority = priority;
        changed();
    }

    @Override
    public synchronized void setCacheMode(int mode) {
        mCacheMode = mode;
        changed();
    }

    @Override
    public synchronized int getCacheMode() {
        return mCacheMode;
    }

    @Override
    public synchronized void setMixedContentMode(int mode) {
        mMixedContentMode = mode;
        changed();
    }

    @Override
    public synchronized int getMixedContentMode() {
        return mMixedContentMode;
    }

    @Override
    public synchronized void setVideoOverlayForEmbeddedEncryptedVideoEnabled(boolean flag) {
        mVideoOverlayForEmbeddedEncryptedVideoEnabled = flag;
        changed();
    }

    @Override
    public synchronized boolean getVideoOverlayForEmbeddedEncryptedVideoEnabled() {
        return mVideoOverlayForEmbeddedEncryptedVideoEnabled;
    }

    @Override
    public synchronized void setOffscreenPreRaster(boolean enabled) {
        mOffscreenPreRaster = enabled;
        changed();
    }

    @Override
    public synchronized boolean getOffscreenPreRaster() {
        return mOffscreenPreRaster;
    }

    @Override
    public synchronized void setSafeBrowsingEnabled(boolean enabled) {
        mSafeBrowsingEnabled = enabled;
        changed();
    }

    @Override
    public synchronized boolean getSafeBrowsingEnabled() {
        return mSafeBrowsingEnabled;
    }

    @Override
    public synchronized void setDisabledActionModeMenuItems(int menuItems) {
        mDisabledActionModeMenuItems = menuItems;
        changed();
    }

    @Override
    public synchronized int getDisabledActionModeMenuItems() {
        return mDisabledActionModeMenuItems;
    }
}

