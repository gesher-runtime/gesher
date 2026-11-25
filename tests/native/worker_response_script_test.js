// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Execute the exact embedded scripts with isolated adapters. These checks prove
// observation/control handling, never native WebView2 cancellation or timing.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../../probes/webview/worker_response_server.cc'), 'utf8');
const workerScript = source.match(/constexpr char kWorkerScript\[\] = R"JS\(([\s\S]*?)\)JS";/)[1];
const pageScript = source.match(/constexpr char kPage\[\] = R"HTML\([\s\S]*?<script>([\s\S]*?)<\/script>\)HTML";/)[1];
const origin = 'http://127.0.0.1:32123';
const cases = ['pass', 'fulfill', 'http403', 'first-read', 'first-abort', 'mid-body',
               'uri-deferred', 'invoke-abort', 'followup'];
let checks = 0;

function timers() {
  const pending = new Map();
  let next = 0;
  return {
    pending,
    setTimeout(callback, milliseconds) {
      const id = ++next;
      pending.set(id, {callback, milliseconds});
      if (milliseconds === 0) queueMicrotask(() => {
        if (pending.delete(id)) callback();
      });
      return id;
    },
    clearTimeout(id) { pending.delete(id); }
  };
}

function response(request, text, status = 200, originHeader = 'origin', failAfter = false) {
  let reads = 0;
  return {
    status, url: request.url,
    headers: new Headers({'X-Probe-Origin': originHeader}),
    body: {getReader: () => ({
      async read() {
        if (++reads === 1) return {done: false, value: new TextEncoder().encode(text)};
        if (failAfter) throw new TypeError('native body read failed');
        return {done: true};
      },
      async cancel() {}
    })}
  };
}

async function runWorker(name, behavior, {timingName, noTiming = false} = {}) {
  const clock = timers(), handlers = new Map(), requests = [];
  let observed, work, entries = [], clears = 0;
  const context = vm.createContext({
    URL, Request, AbortController, TextDecoder,
    crypto: {randomUUID: () => 'e466a13e-3852-49d2-bb22-7adb6bc53cda'},
    performance: {
      clearResourceTimings() { ++clears; entries = []; },
      getEntriesByType(type) { assert.equal(type, 'resource'); return entries; }
    },
    self: {location: {href: origin + '/sw.js'}, addEventListener: (event, callback) => handlers.set(event, callback)},
    setTimeout: clock.setTimeout, clearTimeout: clock.clearTimeout,
    fetch: async request => {
      requests.push(request);
      if (!noTiming) entries.push({name: timingName || request.url, initiatorType: 'fetch'});
      return behavior(request, clock);
    }
  });
  vm.runInContext(workerScript, context);
  handlers.get('message')({data: name, ports: [{postMessage: value => { observed = value; }}],
                            waitUntil: promise => { work = promise; }});
  await work;
  assert.equal(requests.length, 1, 'one real script fetch per case');
  assert.equal(requests[0].url, origin + '/network/' + name);
  assert.equal(requests[0].cache, 'no-store');
  assert.equal(clears, 1, 'timing evidence must not include the previous case');
  assert.equal(observed.requestUrl, requests[0].url);
  assert.equal(observed.finalRequestUrl, requests[0].url);
  assert.equal(observed.workerScript, origin + '/sw.js');
  assert.equal(clock.pending.size, 0, 'deadline and timing timers must be released');
  checks += 8;
  return observed;
}

async function runPage(wrongName = '') {
  const clock = timers(), messages = [], dispatched = [];
  let unregisters = 0, closedPorts = 0;
  class Channel {
    constructor() {
      this.port1 = {onmessage: null, close() { ++closedPorts; }};
      this.port2 = {deliver: data => this.port1.onmessage({data})};
    }
  }
  const worker = {
    state: 'activated', scriptURL: origin + '/sw.js',
    addEventListener() {}, removeEventListener() {},
    postMessage(name, ports) {
      dispatched.push(name);
      queueMicrotask(() => ports[0].deliver({event: 'case', name: name === wrongName ? 'wrong' : name}));
    }
  };
  const registration = {active: worker, scope: origin + '/', async unregister() { ++unregisters; return true; }};
  const context = vm.createContext({
    MessageChannel: Channel, isSecureContext: true,
    setTimeout: clock.setTimeout, clearTimeout: clock.clearTimeout,
    chrome: {webview: {postMessage: value => messages.push(value)}},
    navigator: {serviceWorker: {async register(url, options) {
      assert.equal(url, '/sw.js'); assert.equal(options.scope, '/'); return registration;
    }}},
    async fetch(url, options) {
      assert.equal(url, '/network/document'); assert.equal(options.cache, 'no-store');
      return {status: 200, async text() { return 'origin:document'; }};
    }
  });
  await vm.runInContext(pageScript, context);
  assert.equal(unregisters, 1);
  assert.equal(closedPorts, dispatched.length);
  assert.equal(clock.pending.size, 0);
  checks += 3;
  return {messages, dispatched};
}

async function main() {
  for (const [name, body, status, header] of [
    ['pass', 'origin:pass', 200, 'origin'], ['fulfill', 'supplied', 200, 'intercepted'],
    ['http403', 'denied', 403, 'intercepted'], ['followup', 'origin:followup', 200, 'origin']
  ]) {
    const out = await runWorker(name, request => response(request, body, status, header));
    assert.equal(out.fetch, 'resolved'); assert.equal(out.body, 'complete');
    assert.equal(out.status, status); assert.equal(out.text, body);
    assert.equal(out.errorName, ''); assert.equal(out.originHeader, header);
    assert.equal(out.timedOut, false); assert.equal(out.aborted, false);
    assert.equal(out.timingCount, 1); assert.equal(out.timingName, origin + '/network/' + name);
    checks += 10;
  }
  for (const name of ['uri-deferred', 'invoke-abort']) {
    const out = await runWorker(name, () => { throw new TypeError('Failed to fetch'); });
    assert.equal(out.fetch, 'rejected'); assert.equal(out.body, 'none');
    assert.equal(out.status, 0); assert.equal(out.bytes, 0); assert.equal(out.responseUrl, '');
    assert.equal(out.errorName, 'TypeError'); assert.equal(out.timedOut, false); assert.equal(out.aborted, false);
    checks += 8;
  }
  const prefix = await runWorker('mid-body', request => response(request, 'A'.repeat(65536), 200, 'intercepted', true));
  assert.equal(prefix.fetch, 'resolved', 'body errors must not become initial-fetch rejections');
  assert.equal(prefix.body, 'rejected'); assert.equal(prefix.bytes, 65536); assert.equal(prefix.errorName, 'TypeError');
  checks += 4;
  const timeout = await runWorker('uri-deferred', (request, clock) => new Promise((resolve, reject) => {
    request.signal.addEventListener('abort', () => reject(new DOMException('deadline', 'AbortError')), {once: true});
    queueMicrotask(() => [...clock.pending.values()].find(timer => timer.milliseconds === 10000).callback());
  }));
  assert.equal(timeout.fetch, 'rejected'); assert.equal(timeout.errorName, 'AbortError');
  assert.equal(timeout.timedOut, true); assert.equal(timeout.aborted, true);
  checks += 4;
  const redirected = await runWorker('uri-deferred', () => { throw new TypeError('Failed to fetch'); },
                                     {timingName: 'gesher-denial-unrelated:blocked'});
  assert.equal(redirected.timingName, 'gesher-denial-unrelated:blocked', 'do not replace observed timing with input URL');
  const absent = await runWorker('uri-deferred', () => { throw new TypeError('Failed to fetch'); }, {noTiming: true});
  assert.equal(absent.timingCount, 0); assert.equal(absent.timingName, '');
  checks += 3;
  const page = await runPage();
  assert.deepEqual(page.dispatched, cases, 'all baseline, denial and follow-up cases must actually be dispatched');
  assert.equal(page.messages.at(-1).event, 'complete'); assert.equal(page.messages.at(-1).unregistered, true);
  const bad = await runPage('uri-deferred');
  assert.equal(bad.messages.at(-1).event, 'failure');
  assert.match(bad.messages.at(-1).error, /wrong worker result/);
  assert.equal(bad.messages.some(message => message.event === 'complete'), false);
  checks += 6;
  console.log(`Worker response scripts: ${checks} checks passed; isolated adapters, no WebView2 qualification`);
}
main().catch(error => { console.error(error); process.exitCode = 1; });
