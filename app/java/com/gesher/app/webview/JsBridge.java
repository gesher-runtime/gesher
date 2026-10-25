package com.gesher.app.webview;

import android.util.Log;
import android.webkit.JavascriptInterface;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeSet;

/**
 * WebView.addJavascriptInterface over WebView2 (docs/webview.md, "JavaScript
 * interfaces"). A script run in every new document defines window.<name> with one
 * function per method; each calls callJava on the host object {@code gesherBridge} (an IDispatch,
 * compat/webview/webview_host.cc) synchronously with the interface's and the method's
 * names and the arguments as JSON, and the answer comes back as JSON: {"v": value} or
 * {"e": message}, which the function throws. Only public methods annotated
 * {@code @JavascriptInterface} are callable (all public methods for apps targeting API
 * 16 and lower, as on Android); they run on the WebView thread, not the UI thread, as
 * Android runs them on its JavaBridge thread.
 */
final class JsBridge {
    private static final String TAG = "gesher-webview";

    private JsBridge() {
    }

    /** The document script for these interfaces (empty when there are none). */
    static String script(Map<String, Object> interfaces, boolean requireAnnotation) {
        JSONObject defs = new JSONObject();
        try {
            for (Map.Entry<String, Object> e : interfaces.entrySet()) {
                if (e.getValue() == null) continue;
                JSONArray names = new JSONArray();
                for (String m : methodNames(e.getValue().getClass(), requireAnnotation)) {
                    names.put(m);
                }
                defs.put(e.getKey(), names);
            }
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
        if (defs.length() == 0) return "";
        return "(function(){\n"
                + "var defs = " + defs + ";\n"
                + "var bridge = null;\n"
                + "function invoke(name, m, args) {\n"
                + "  if (!bridge) bridge = chrome.webview.hostObjects.sync.gesherBridge;\n"
                + "  var r = JSON.parse(bridge.callJava(name, m,"
                + " JSON.stringify(Array.prototype.slice.call(args))));\n"
                + "  if (r.hasOwnProperty('e')) throw new Error(r.e);\n"
                + "  return r.v === null ? (r.u ? undefined : null) : r.v;\n"
                + "}\n"
                + "Object.keys(defs).forEach(function(name) {\n"
                + "  var o = {};\n"
                + "  defs[name].forEach(function(m) {\n"
                + "    o[m] = function() { return invoke(name, m, arguments); };\n"
                + "  });\n"
                + "  try { Object.defineProperty(window, name,"
                + " {value: o, configurable: true, writable: true, enumerable: false}); }\n"
                + "  catch (e) { window[name] = o; }\n"
                + "});\n"
                + "})();\n";
    }

    private static boolean callable(Method m, boolean requireAnnotation) {
        if (!Modifier.isPublic(m.getModifiers()) || Modifier.isStatic(m.getModifiers())) {
            return false;
        }
        if (m.getDeclaringClass() == Object.class) return false;
        return !requireAnnotation || m.isAnnotationPresent(JavascriptInterface.class);
    }

    private static TreeSet<String> methodNames(Class<?> c, boolean requireAnnotation) {
        TreeSet<String> names = new TreeSet<>();
        for (Method m : c.getMethods()) {
            if (callable(m, requireAnnotation)) names.add(m.getName());
        }
        return names;
    }

    /** Calls interfaceName.method(args...) and answers as the script expects. */
    static String call(Map<String, Object> interfaces, boolean requireAnnotation,
            String interfaceName, String method, String argsJson) {
        Object target = interfaces.get(interfaceName);
        if (target == null) return error("no interface " + interfaceName);
        JSONArray args;
        try {
            args = new JSONArray(argsJson == null || argsJson.isEmpty() ? "[]" : argsJson);
        } catch (JSONException e) {
            return error("bad arguments");
        }
        List<Method> candidates = new ArrayList<>();
        for (Method m : target.getClass().getMethods()) {
            if (m.getName().equals(method) && m.getParameterCount() == args.length()
                    && callable(m, requireAnnotation)) {
                candidates.add(m);
            }
        }
        if (candidates.isEmpty()) return error("Method not found");
        Method m = candidates.get(0);
        Object[] values = new Object[args.length()];
        Class<?>[] types = m.getParameterTypes();
        for (int i = 0; i < values.length; i++) {
            values[i] = convert(args.opt(i), types[i]);
        }
        Object result;
        try {
            m.setAccessible(true);
            result = m.invoke(target, values);
        } catch (InvocationTargetException e) {
            Log.e(TAG, "a JavaScript interface method threw", e.getCause());
            return error("Java exception was raised during method invocation");
        } catch (ReflectiveOperationException | IllegalArgumentException e) {
            return error(String.valueOf(e));
        }
        if (m.getReturnType() == void.class) return "{\"v\":null,\"u\":true}";
        return "{\"v\":" + toJson(result) + "}";
    }

    private static String error(String message) {
        return "{\"e\":" + JSONObject.quote(message) + "}";
    }

    private static String toJson(Object v) {
        if (v == null) return "null";
        if (v instanceof String || v instanceof Character) return JSONObject.quote(v.toString());
        if (v instanceof Boolean) return v.toString();
        if (v instanceof Number) {
            double d = ((Number) v).doubleValue();
            if (Double.isNaN(d) || Double.isInfinite(d)) return "null";
            try {
                return JSONObject.numberToString((Number) v);
            } catch (JSONException e) {
                return "null";
            }
        }
        // Android hands back a wrapper for other objects; Gesher gives undefined.
        return "null";
    }

    private static Object convert(Object v, Class<?> type) {
        boolean missing = v == null || v == JSONObject.NULL;
        if (type == String.class) {
            if (missing) return null;
            return v instanceof String ? v : String.valueOf(v);
        }
        Number n = v instanceof Number ? (Number) v : null;
        if (n == null && v instanceof String) {
            try {
                n = Double.parseDouble((String) v);
            } catch (NumberFormatException e) {
                n = null;
            }
        }
        if (type == int.class || type == Integer.class) return n != null ? n.intValue() : 0;
        if (type == long.class || type == Long.class) return n != null ? n.longValue() : 0L;
        if (type == double.class || type == Double.class) return n != null ? n.doubleValue() : 0.0;
        if (type == float.class || type == Float.class) return n != null ? n.floatValue() : 0f;
        if (type == short.class || type == Short.class) return n != null ? n.shortValue() : (short) 0;
        if (type == byte.class || type == Byte.class) return n != null ? n.byteValue() : (byte) 0;
        if (type == boolean.class || type == Boolean.class) {
            if (v instanceof Boolean) return v;
            return n != null && n.doubleValue() != 0;
        }
        if (type == char.class || type == Character.class) {
            if (v instanceof String && !((String) v).isEmpty()) return ((String) v).charAt(0);
            return n != null ? (char) n.intValue() : '\0';
        }
        return missing ? null : (type.isInstance(v) ? v : null);
    }
}
