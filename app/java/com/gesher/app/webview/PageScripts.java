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
            + "function channels(ids) {\n"
            + "  return (ids || []).map(function(id) {\n"
            + "    var ch = new MessageChannel();\n"
            + "    ports[id] = ch.port2;\n"
            + "    ch.port2.onmessage = function(e) {\n"
            + "      chrome.webview.postMessage({gesherPort: id,"
            + " data: typeof e.data === 'string' ? e.data : JSON.stringify(e.data)});\n"
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
            + " {data: m.data, origin: '', ports: channels(m.ports)}));\n"
            + "  } else if (m.gesher === 'port') {\n"
            + "    var p = ports[m.id];\n"
            + "    if (p) p.postMessage(m.data, channels(m.ports));\n"
            + "  } else if (m.gesher === 'close') {\n"
            + "    var q = ports[m.id];\n"
            + "    if (q) { q.close(); delete ports[m.id]; }\n"
            + "  }\n"
            + "});\n"
            + "})();\n";

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
