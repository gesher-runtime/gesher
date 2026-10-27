package com.gesher.app.webview;

import android.util.Base64;
import android.webkit.WebMessage;
import android.webkit.WebMessagePort;
import org.json.JSONException;
import org.json.JSONObject;

/** A typed message inside the framework's string-only WebMessage API. */
final class WebMessagePayload extends WebMessage {
    private final byte[] mBytes;

    WebMessagePayload(byte[] bytes, WebMessagePort[] ports) {
        super(null, ports);
        if (bytes == null) throw new NullPointerException("arrayBuffer");
        mBytes = bytes.clone();
    }

    static byte[] bytes(WebMessage message) {
        return message instanceof WebMessagePayload ? ((WebMessagePayload) message).mBytes.clone() : null;
    }

    static boolean binary(WebMessage message) {
        return message instanceof WebMessagePayload;
    }

    static void write(JSONObject object, WebMessage message) throws JSONException {
        byte[] bytes = binary(message) ? ((WebMessagePayload) message).mBytes : null;
        object.put("data", bytes != null ? Base64.encodeToString(bytes, Base64.NO_WRAP)
                : message.getData() != null ? message.getData() : "");
        if (bytes != null) object.put("binary", true);
    }

    static WebMessage read(JSONObject object) throws JSONException {
        if (!object.optBoolean("binary")) return new WebMessage(object.optString("data"));
        try {
            return new WebMessagePayload(Base64.decode(object.getString("data"), Base64.DEFAULT), null);
        } catch (IllegalArgumentException invalid) {
            throw new JSONException("Invalid ArrayBuffer payload");
        }
    }
}
