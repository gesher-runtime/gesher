package com.gesher.app.webview;

import org.json.JSONObject;

/**
 * Scripts Gesher runs in pages (docs/webview.md): the page side of WebMessagePorts (part
 * of every document's script), text zoom, and find in page where WebView2's Find API is
 * missing.
 */
final class PageScripts {
    private PageScripts() {
    }

    /**
     * The page side of WebMessagePorts (GesherWebMessagePort). The app's messages come as
     * chrome.webview message events: {gesher: "post", data, origin, ports: [ids]} is a
     * window message event whose ports are new MessageChannels' port1 (port2 stays here
     * under the id); {gesher: "port", id, data, ports} posts data to port2 of that id (its
     * port1 in the page receives it); {gesher: "close", id} closes it. What the page posts
     * to a port1 goes back as chrome.webview.postMessage({gesherPort: id, data}).
     */
    static final String PORTS = "(function(){\n"
            + "if (window.top !== window || !window.chrome || !chrome.webview"
            + " || window.__gesherPorts) return;\n"
            + "var ports = {};\n"
            + "Object.defineProperty(window, '__gesherPorts', {value: ports});\n"
            + "var byteLength = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, 'byteLength').get;\n"
            + "function isBuffer(data) { try { byteLength.call(data); return true; } catch (e) { return false; } }\n"
            + "function encode(data) {\n"
            + " if (!isBuffer(data)) return {data: typeof data === 'string' ? data : JSON.stringify(data)};\n"
            + " var bytes = new Uint8Array(data), text = '';\n"
            + " for (var i = 0; i < bytes.length; i += 8192)"
            + " text += String.fromCharCode.apply(null, bytes.subarray(i, i + 8192));\n"
            + " return {data: btoa(text), binary: true};\n"
            + "}\n"
            + "function decode(message) {\n"
            + " if (!message.binary) return message.data;\n"
            + " var text = atob(message.data), bytes = new Uint8Array(text.length);\n"
            + " for (var i = 0; i < text.length; i++) bytes[i] = text.charCodeAt(i);\n"
            + " return bytes.buffer;\n"
            + "}\n"
            + "Object.defineProperty(window, '__gesherEncodeMessage', {value: encode});\n"
            + "Object.defineProperty(window, '__gesherDecodeMessage', {value: decode});\n"
            + "Object.defineProperty(window, '__gesherIsArrayBuffer', {value: isBuffer});\n"
            + "function channels(ids) {\n"
            + "  return (ids || []).map(function(id) {\n"
            + "    var ch = new MessageChannel();\n"
            + "    ports[id] = ch.port2;\n"
            + "    ch.port2.onmessage = function(e) {\n"
            + "      chrome.webview.postMessage(Object.assign({gesherPort: id}, encode(e.data)));\n"
            + "    };\n"
            + "    return ch.port1;\n"
            + "  });\n"
            + "}\n"
            + "chrome.webview.addEventListener('message', function(e) {\n"
            + "  var m = e.data;\n"
            + "  if (!m || typeof m !== 'object' || !m.gesher) return;\n"
            + "  if (m.gesher === 'post') {\n"
            + "    if (m.origin && m.origin !== '*' && m.origin !== location.origin) return;\n"
            + "    window.dispatchEvent(new MessageEvent('message',"
            + " {data: decode(m), origin: '', ports: channels(m.ports)}));\n"
            + "  } else if (m.gesher === 'port') {\n"
            + "    var p = ports[m.id];\n"
            + "    if (p) p.postMessage(decode(m), channels(m.ports));\n"
            + "  } else if (m.gesher === 'close') {\n"
            + "    var q = ports[m.id];\n"
            + "    if (q) { q.close(); delete ports[m.id]; }\n"
            + "  }\n"
            + "});\n"
            + "})();\n";

    /**
     * Android's allowed origin rules (androidx.webkit's WebMessageListener and document-start
     * scripts, ADR 0058): {@code window.__gesherOriginAllowed(rules)} is true when the
     * document's origin matches one: "*", or scheme://host[:port] with an optional "*." host
     * prefix (subdomains). Pages loaded from file: URLs (FileUrls' origin) are file://.
     */
    static final String ORIGINS = "(function(){\n"
            + "if (window.__gesherOriginAllowed) return;\n"
            + "var fileOrigin = " + JSONObject.quote(FileUrls.toWebView2("file:///")
                    .replaceAll("/+$", "")) + ";\n"
            + "var defaults = {'http': '80', 'https': '443'};\n"
            + "function allowed(rules) {\n"
            + "  var scheme, host, port;\n"
            + "  if (location.origin === fileOrigin) { scheme = 'file'; host = ''; port = ''; }\n"
            + "  else { scheme = location.protocol.replace(/:$/, ''); host = location.hostname;"
            + " port = location.port || defaults[scheme] || ''; }\n"
            + "  for (var i = 0; i < rules.length; i++) {\n"
            + "    var r = rules[i];\n"
            + "    if (r === '*') return true;\n"
            + "    var m = /^([a-zA-Z][a-zA-Z0-9+.-]*):\\/\\/(\\*\\.)?([^:\\/]*)(?::(\\d+))?\\/?$/.exec(r);\n"
            + "    if (!m || m[1].toLowerCase() !== scheme) continue;\n"
            + "    var h = m[3].toLowerCase();\n"
            + "    if (m[2] ? !host.endsWith('.' + h) : host !== h) continue;\n"
            + "    if ((m[4] || defaults[scheme] || '') !== port) continue;\n"
            + "    return true;\n"
            + "  }\n"
            + "  return false;\n"
            + "}\n"
            + "Object.defineProperty(window, '__gesherOriginAllowed', {value: allowed});\n"
            + "})();\n";

    /**
     * The page side of an androidx.webkit WebMessageListener (ADR 0058):
     * {@code window[name]} with postMessage, onmessage and add/removeEventListener, in main
     * frames whose origin a rule allows. The page's messages go to the app as
     * chrome.webview.postMessage({gesherListener: name, data}); the app's replies
     * (JavaScriptReplyProxy) come as {gesher: "listener", name, data} message events.
     */
    static String messageListener(String name, String[] rules) {
        return "(function(name, rules){\n"
                + "if (!window.chrome || !chrome.webview"
                + " || !window.__gesherOriginAllowed(rules)) return;\n"
                + "var listeners = [], obj = {onmessage: null};\n"
                + "obj.postMessage = function(data) {\n"
                + "  if (arguments.length < 1) throw new TypeError('postMessage: 1 argument required');\n"
                + "  if (typeof data !== 'string' && !window.__gesherIsArrayBuffer(data))"
                + " throw new TypeError('Expected string or ArrayBuffer');\n"
                + "  chrome.webview.postMessage(Object.assign({gesherListener: name}, window.__gesherEncodeMessage(data)));\n"
                + "};\n"
                + "obj.addEventListener = function(type, f) {"
                + " if (type === 'message' && typeof f === 'function' && listeners.indexOf(f) < 0)"
                + " listeners.push(f); };\n"
                + "obj.removeEventListener = function(type, f) {"
                + " var i = listeners.indexOf(f); if (type === 'message' && i >= 0) listeners.splice(i, 1); };\n"
                + "Object.defineProperty(window, name, {value: obj, configurable: true});\n"
                + "chrome.webview.addEventListener('message', function(e) {\n"
                + "  var m = e.data;\n"
                + "  if (!m || typeof m !== 'object' || m.gesher !== 'listener' || m.name !== name) return;\n"
                + "  var ev = new MessageEvent('message', {data: window.__gesherDecodeMessage(m)});\n"
                + "  if (typeof obj.onmessage === 'function') obj.onmessage(ev);\n"
                + "  listeners.slice().forEach(function(f) { f(ev); });\n"
                + "});\n"
                + "})(" + JSONObject.quote(name) + ", " + new org.json.JSONArray(
                        java.util.Arrays.asList(rules)) + ");\n";
    }

    /**
     * An app's document-start script (androidx.webkit's addDocumentStartJavaScript), run
     * where a rule allows the origin. A block, not a function: its var and function
     * declarations stay global, as the app's script run on its own.
     */
    static String documentStartScript(String script, String[] rules) {
        return "if (window.__gesherOriginAllowed(" + new org.json.JSONArray(
                java.util.Arrays.asList(rules)) + ")) {\n" + script + "\n}\n";
    }

    /**
     * Text zoom (WebSettings.setTextZoom): every element's font size times the factor, the
     * layout's other lengths as they are (Android's text zoom, unlike WebView2's page zoom).
     * The elements' own font sizes are read once with the factor undone, then set as
     * important inline sizes; nodes added later are scaled after they are added.
     * {@code window.__gesherTextZoom(f)} changes the factor.
     */
    static String textZoom(int percent) {
        return "(function(){\n"
                + "if (window.__gesherTextZoom) { window.__gesherTextZoom(" + (percent / 100.0)
                + "); return; }\n"
                + "var factor = " + (percent / 100.0) + ", touched = [], timer = 0, observer = null;\n"
                + "function restore() {\n"
                + "  for (var i = 0; i < touched.length; i++) {\n"
                + "    var t = touched[i];\n"
                + "    if (t[1]) t[0].style.setProperty('font-size', t[1], t[2]);\n"
                + "    else t[0].style.removeProperty('font-size');\n"
                + "  }\n"
                + "  touched = [];\n"
                + "}\n"
                + "function apply() {\n"
                + "  timer = 0;\n"
                + "  if (observer) observer.disconnect();\n"
                + "  restore();\n"
                + "  if (factor !== 1 && document.documentElement) {\n"
                + "    var els = document.getElementsByTagName('*'), sizes = [];\n"
                + "    for (var i = 0; i < els.length; i++)"
                + " sizes.push(parseFloat(getComputedStyle(els[i]).fontSize) || 0);\n"
                + "    for (var j = 0; j < els.length; j++) {\n"
                + "      var el = els[j];\n"
                + "      if (!el.style || !sizes[j]) continue;\n"
                + "      touched.push([el, el.style.getPropertyValue('font-size'),"
                + " el.style.getPropertyPriority('font-size')]);\n"
                + "      el.style.setProperty('font-size', (sizes[j] * factor) + 'px', 'important');\n"
                + "    }\n"
                + "  }\n"
                + "  if (factor !== 1 && document.documentElement) {\n"
                + "    observer = observer || new MutationObserver(function() {"
                + " if (!timer) timer = setTimeout(apply, 50); });\n"
                + "    observer.observe(document.documentElement, {childList: true, subtree: true});\n"
                + "  }\n"
                + "}\n"
                + "Object.defineProperty(window, '__gesherTextZoom', {value: function(f) {"
                + " factor = f; apply(); }});\n"
                + "if (factor !== 1) {\n"
                + "  if (document.readyState === 'loading')"
                + " document.addEventListener('DOMContentLoaded', apply);\n"
                + "  else apply();\n"
                + "}\n"
                + "})();\n";
    }

    /**
     * Find in page without WebView2's Find API: the matches of {@code term} (case-insensitive)
     * in the document's text, highlighted with the CSS Custom Highlight API where there is
     * one, the active one selected and scrolled to. op: "start", "next", "previous" or
     * "clear"; the result is {"count": n, "active": index (0-based, -1 none)}.
     */
    static String find(String op, String term) {
        return "(function(op, term){\n"
                + "var s = window.__gesherFind || (window.__gesherFind = {ranges: [], active: -1});\n"
                + "function select() {\n"
                + "  var sel = window.getSelection();\n"
                + "  sel.removeAllRanges();\n"
                + "  if (s.active < 0) return;\n"
                + "  var r = s.ranges[s.active];\n"
                + "  sel.addRange(r);\n"
                + "  var e = r.startContainer.parentElement;\n"
                + "  if (e && e.scrollIntoView) e.scrollIntoView({block: 'center'});\n"
                + "}\n"
                + "if (op === 'start' || op === 'clear') {\n"
                + "  s.ranges = []; s.active = -1;\n"
                + "  if (window.CSS && CSS.highlights) CSS.highlights.delete('gesher-find');\n"
                + "  if (op === 'clear' || !term) { window.getSelection().removeAllRanges();"
                + " return {count: 0, active: -1}; }\n"
                + "  var needle = term.toLowerCase();\n"
                + "  var walker = document.createTreeWalker(document.body || document.documentElement,"
                + " NodeFilter.SHOW_TEXT, {acceptNode: function(n) {\n"
                + "    var p = n.parentElement;\n"
                + "    return p && !/^(SCRIPT|STYLE|NOSCRIPT)$/.test(p.tagName)"
                + " ? NodeFilter.FILTER_ACCEPT : NodeFilter.FILTER_REJECT; }});\n"
                + "  for (var n = walker.nextNode(); n; n = walker.nextNode()) {\n"
                + "    var text = n.data.toLowerCase();\n"
                + "    for (var i = text.indexOf(needle); i >= 0; i = text.indexOf(needle, i + needle.length)) {\n"
                + "      var r = document.createRange();\n"
                + "      r.setStart(n, i); r.setEnd(n, i + needle.length);\n"
                + "      s.ranges.push(r);\n"
                + "    }\n"
                + "  }\n"
                + "  if (window.CSS && CSS.highlights && window.Highlight && s.ranges.length) {\n"
                + "    CSS.highlights.set('gesher-find', new Highlight(...s.ranges));\n"
                + "    if (!document.getElementById('gesher-find-style')) {\n"
                + "      var st = document.createElement('style');\n"
                + "      st.id = 'gesher-find-style';\n"
                + "      st.textContent = '::highlight(gesher-find) { background-color: #ffff00; }';\n"
                + "      (document.head || document.documentElement).appendChild(st);\n"
                + "    }\n"
                + "  }\n"
                + "  s.active = s.ranges.length ? 0 : -1;\n"
                + "} else if (s.ranges.length) {\n"
                + "  var n2 = s.ranges.length;\n"
                + "  s.active = op === 'next' ? (s.active + 1) % n2 : (s.active - 1 + n2) % n2;\n"
                + "}\n"
                + "select();\n"
                + "return {count: s.ranges.length, active: s.active};\n"
                + "})(" + JSONObject.quote(op) + ", " + JSONObject.quote(term != null ? term : "")
                + ")";
    }
}
