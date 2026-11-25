// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Isolated scripts for the real-worker denial scope matrix.
#pragma once
namespace worker_denial_probe {
inline constexpr char kWorker[] = R"JS(
'use strict';
const workerIdentity = crypto.randomUUID();
self.addEventListener('install', event => event.waitUntil(self.skipWaiting()));
self.addEventListener('activate', event => event.waitUntil(self.clients.claim()));
self.addEventListener('message', event => event.waitUntil((async () => {
  const {action, other} = event.data;
  const own = self.location.origin;
  const specs = action === 'parallel' ? ['parallel-allow', 'parallel-deny'] : [action];
  performance.clearResourceTimings();
  const observations = await Promise.all(specs.map(async name => {
    const cross = name.startsWith('cors-');
    const concurrent = name.startsWith('parallel-') || name === 'sequential-pass';
    const method = name.startsWith('redirect-') || name === 'teardown-followup' ? 'GET' : 'POST';
    const url = (cross ? other : own) + '/denial/network/' + (concurrent ? 'concurrent' : name);
    const token = name + '|' + workerIdentity + '|';
    const payload = method === 'POST' ? token + 'P'.repeat(32768 - token.length) : undefined;
    const abort = new AbortController();
    const headers = method === 'POST' ? {'Content-Type': cross ? 'application/json' : 'text/plain',
                                        'X-Gesher-Diagnostic': name} : undefined;
    const request = new Request(url, {method, headers, body:payload, mode:'cors', cache:'no-store', signal:abort.signal});
    const out = {event:'matrix-case', action, name, workerIdentity, workerScript:self.location.href,
                 requestUrl:request.url, finalRequestUrl:'', method, finalMethod:'', uploadBytes:payload ? payload.length : 0,
                 fetch:'pending', body:'none', status:0, bytes:0, text:'', errorName:'', error:'',
                 responseUrl:'', responseType:'', redirected:false, originHeader:'', timedOut:false, aborted:false};
    const timer = setTimeout(() => { out.timedOut = true; abort.abort(); }, 10000);
    try {
      let response;
      try { response = await fetch(request); }
      catch (error) { out.fetch = 'rejected'; out.errorName = String(error.name); out.error = String(error).slice(0, 512); }
      if (response) {
        out.fetch = 'resolved'; out.status = response.status; out.responseUrl = response.url;
        out.responseType = response.type; out.redirected = response.redirected;
        out.originHeader = response.headers.get('X-Probe-Origin') || '';
        try {
          const reader = response.body.getReader();
          const decoder = new TextDecoder();
          for (;;) {
            const part = await reader.read();
            if (part.done) break;
            out.bytes += part.value.byteLength;
            if (out.bytes > 262144) { await reader.cancel(); throw Error('body limit'); }
            if (out.text.length < 128) out.text += decoder.decode(part.value).slice(0, 128 - out.text.length);
          }
          out.body = 'complete';
        } catch (error) { out.body = 'rejected'; out.errorName = String(error.name); out.error = String(error).slice(0, 512); }
      }
    } finally {
      clearTimeout(timer);
      out.aborted = abort.signal.aborted;
      out.finalRequestUrl = request.url; out.finalMethod = request.method;
    }
    return out;
  }));
  await new Promise(resolve => setTimeout(resolve, 0));
  const timings = performance.getEntriesByType('resource').slice(0, 16).map(item => ({name:item.name, type:item.initiatorType}));
  for (const item of observations) item.timings = timings;
  event.ports[0].postMessage({action, observations});
})()));
)JS";

inline constexpr char kPage[] = R"HTML(<!doctype html><meta charset="utf-8"><title>worker denial scope</title>
<script>
'use strict';
(async () => {
  const post = value => chrome.webview.postMessage(value);
  const observer = location.pathname === '/denial/observer.html';
  const other = new URL(location.href).searchParams.get('other');
  const within = (promise, ms) => new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(Error('operation timeout')), ms);
    promise.then(value => { clearTimeout(timer); resolve(value); }, error => { clearTimeout(timer); reject(error); });
  });
  let registration;
  try {
    registration = await within(observer ? navigator.serviceWorker.getRegistration('/denial/') :
      navigator.serviceWorker.register('/denial/sw.js', {scope:'/denial/'}), 15000);
    if (!registration) throw Error('registration unavailable');
    const worker = registration.active || registration.waiting || registration.installing;
    if (!worker) throw Error('actual worker unavailable');
    await new Promise((resolve, reject) => {
      const finish = error => { clearTimeout(timer); worker.removeEventListener('statechange', changed); error ? reject(error) : resolve(); };
      const changed = () => {
        if (worker.state === 'activated') finish();
        else if (worker.state === 'redundant') finish(Error('worker redundant'));
      };
      const timer = setTimeout(() => finish(Error('activation timeout')), 15000);
      worker.addEventListener('statechange', changed); changed();
    });
    post({event:'matrix-ready', observer, secure:isSecureContext, script:worker.scriptURL, scope:registration.scope});
    const actions = observer ? ['teardown-deny', 'teardown-followup'] :
      ['post-pass', 'post-deny', 'cors-pass', 'cors-deny', 'redirect-pass', 'redirect-deny', 'parallel', 'sequential-pass'];
    for (const action of actions) {
      const group = await new Promise((resolve, reject) => {
        const channel = new MessageChannel();
        const timer = setTimeout(() => { channel.port1.close(); reject(Error('worker message timeout')); }, 12000);
        channel.port1.onmessage = event => {
          clearTimeout(timer); channel.port1.close();
          if (event.data.action !== action || !Array.isArray(event.data.observations)) reject(Error('wrong worker group'));
          else resolve(event.data);
        };
        worker.postMessage({action, other}, [channel.port2]);
      });
      for (const item of group.observations) post(item);
    }
    if (observer) post({event:'matrix-complete', unregistered:await within(registration.unregister(), 5000)});
    else post({event:'matrix-main-complete'});
  } catch (error) {
    if (registration) await within(registration.unregister(), 5000).catch(() => false);
    post({event:'matrix-failure', error:String(error).slice(0, 512)});
  }
})();
</script>)HTML";
}  // namespace worker_denial_probe
