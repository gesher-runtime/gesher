package com.gesher.app.webview;

import android.graphics.Bitmap;
import android.webkit.WebBackForwardList;
import android.webkit.WebHistoryItem;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * WebView.copyBackForwardList on WebView2 (docs/webview.md): the DevTools protocol's
 * Page.getNavigationHistory, read after each history change; immutable.
 */
final class GesherBackForwardList extends WebBackForwardList {
    static final GesherBackForwardList EMPTY = new GesherBackForwardList(
            Collections.emptyList(), -1);

    private final List<Item> mItems;
    private final int mCurrent;

    private GesherBackForwardList(List<Item> items, int current) {
        mItems = items;
        mCurrent = current;
    }

    /** From Page.getNavigationHistory's result; null when it does not parse. */
    static GesherBackForwardList parse(String json) {
        try {
            JSONObject o = new JSONObject(json);
            JSONArray entries = o.getJSONArray("entries");
            List<Item> items = new ArrayList<>();
            for (int i = 0; i < entries.length(); i++) {
                JSONObject e = entries.getJSONObject(i);
                String url = FileUrls.fromWebView2(e.optString("url"));
                String typed = e.optString("userTypedURL");
                items.add(new Item(e.optInt("id"), url,
                        typed.isEmpty() ? url : FileUrls.fromWebView2(typed), e.optString("title")));
            }
            return new GesherBackForwardList(Collections.unmodifiableList(items),
                    Math.min(o.optInt("currentIndex", items.size() - 1), items.size() - 1));
        } catch (Exception e) {
            return null;
        }
    }

    /** The DevTools entry id at current + steps, or -1. */
    int entryId(int steps) {
        int i = mCurrent + steps;
        return i >= 0 && i < mItems.size() ? mItems.get(i).mId : -1;
    }

    @Override
    public WebHistoryItem getCurrentItem() {
        return mCurrent >= 0 ? mItems.get(mCurrent) : null;
    }

    @Override
    public int getCurrentIndex() {
        return mCurrent;
    }

    @Override
    public WebHistoryItem getItemAtIndex(int index) {
        return index >= 0 && index < mItems.size() ? mItems.get(index) : null;
    }

    @Override
    public int getSize() {
        return mItems.size();
    }

    @Override
    protected WebBackForwardList clone() {
        return this;
    }

    static final class Item extends WebHistoryItem {
        final int mId;
        private final String mUrl;
        private final String mOriginalUrl;
        private final String mTitle;

        Item(int id, String url, String originalUrl, String title) {
            mId = id;
            mUrl = url;
            mOriginalUrl = originalUrl;
            mTitle = title;
        }

        @Override
        @SuppressWarnings("deprecation")
        public int getId() {
            return mId;
        }

        @Override
        public String getUrl() {
            return mUrl;
        }

        @Override
        public String getOriginalUrl() {
            return mOriginalUrl;
        }

        @Override
        public String getTitle() {
            return mTitle;
        }

        @Override
        public Bitmap getFavicon() {
            return null;
        }

        @Override
        protected WebHistoryItem clone() {
            return this;
        }
    }
}
