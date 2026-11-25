// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Exact scope-matrix scripts with isolated adapters, not WebView2 qualification.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../../probes/webview/worker_denial_scripts.h'), 'utf8');
const workerScript = source.match(/char kWorker\[\] = R"JS\(([\s\S]*?)\)JS";/)[1];
const pageScript = source.match(/char kPage\[\] = R"HTML\([\s\S]*?<script>([\s\S]*?)<\/script>\)HTML";/)[1];
const origin = 'http://127.0.0.1:32123', other = 'http://127.0.0.1:32124';
const identity = 'e466a13e-3852-49d2-bb22-7adb6bc53cda';
const actions = ['post-pass', 'post-deny', 'cors-pass', 'cors-deny', 'redirect-pass', 'redirect-deny', 'parallel', 'sequential-pass'];
const names = [...actions.slice(0, 6), 'parallel-allow', 'parallel-deny', 'sequential-pass', 'teardown-deny', 'teardown-followup'];
let checks = 0;
function check(actual, expected, description) { assert.deepEqual(actual, expected, description); ++checks; }

function timers() {
  const pending = new Map(); let next = 0;
  return {
    pending,
    setTimeout(callback, milliseconds) {
      const id = ++next; pending.set(id, {callback, milliseconds});
      if (!milliseconds) queueMicrotask(() => { if (pending.delete(id)) callback(); });
      return id;
    },
    clearTimeout(id) { pending.delete(id); },
    fire(milliseconds) {
      const entry = [...pending].find(([, item]) => item.milliseconds === milliseconds);
      assert.ok(entry, 'fixture deadline exists'); pending.delete(entry[0]); entry[1].callback();
    }
  };
}

function response(request, name, {bodyFailure = false, status = 200} = {}) {
  const suffix = name === 'redirect-pass' ? 'redirect-pass-target' : request.url.split('/').at(-1);
  let reads = 0;
  return {
    status, url: name === 'redirect-pass' ? other + '/denial/network/redirect-pass-target' : request.url,
    type: name.startsWith('cors-') || name === 'redirect-pass' ? 'cors' : 'basic', redirected: name === 'redirect-pass',
    headers: new Headers({'X-Probe-Origin': 'origin'}),
    body: {getReader: () => ({
      async read() {
        if (++reads === 1) return {done: false, value: new TextEncoder().encode('origin:' + suffix)};
        if (bodyFailure) throw new TypeError('body read failed');
        return {done: true};
      },
      async cancel() {}
    })}
  };
}

function worker(behavior, {timingName, noTiming = false} = {}) {
  const clock = timers(), handlers = new Map();
  let entries = [], clears = 0, work, message;
  const requests = [];
  const context = vm.createContext({
    Request, AbortController, TextDecoder, crypto: {randomUUID: () => identity},
    setTimeout: clock.setTimeout, clearTimeout: clock.clearTimeout,
    self: {location: {href: origin + '/denial/sw.js', origin}, addEventListener: (event, callback) => handlers.set(event, callback)},
    performance: {
      clearResourceTimings() { ++clears; entries = []; },
      getEntriesByType(type) { check(type, 'resource'); return entries; }
    },
    async fetch(request) {
      const name = request.headers.get('X-Gesher-Diagnostic') || request.url.split('/').at(-1);
      requests.push(name);
      check(request.cache, 'no-store'); check(request.mode, 'cors');
      const body = await request.clone().text();
      const isPost = !name.startsWith('redirect-') && name !== 'teardown-followup';
      check(request.method, isPost ? 'POST' : 'GET');
      const token = name + '|' + identity + '|';
      check(body, isPost ? token + 'P'.repeat(32768 - token.length) : '', 'the whole upload is attributable to this worker and case');
      if (isPost) check(request.headers.get('Content-Type'), name.startsWith('cors-') ? 'application/json' : 'text/plain');
      if (!noTiming) entries.push({name: timingName || request.url, initiatorType: 'fetch'});
      return behavior(request, name, clock, requests);
    }
  });
  vm.runInContext(workerScript, context);
  return async action => {
    const previous = clears;
    handlers.get('message')({data: {action, other}, ports: [{postMessage: value => { message = value; }}],
                              waitUntil: promise => { work = promise; }});
    await work;
    check(message.action, action); check(clears, previous + 1); check(clock.pending.size, 0);
    const out = JSON.parse(JSON.stringify(message.observations));
    check(out.map(item => item.name), action === 'parallel' ? ['parallel-allow', 'parallel-deny'] : [action]);
    for (const item of out) {
      const url = (item.name.startsWith('cors-') ? other : origin) + '/denial/network/' +
                  (item.name.startsWith('parallel-') || item.name === 'sequential-pass' ? 'concurrent' : item.name);
      check(item.requestUrl, url); check(item.finalRequestUrl, url); check(item.method, item.finalMethod);
      check(item.workerIdentity, identity); check(item.workerScript, origin + '/denial/sw.js');
      check(item.uploadBytes, item.method === 'POST' ? 32768 : 0);
    }
    return out;
  };
}

async function page(observer, {wrongGroup = false, timeout = false, redundant = false} = {}) {
  const clock = timers(), messages = [], dispatched = [];
  let unregisters = 0, registrations = 0, lookups = 0, closedPorts = 0;
  class Channel {
    constructor() {
      this.port1 = {onmessage: null, close() { ++closedPorts; }};
      this.port2 = {deliver: data => this.port1.onmessage({data})};
    }
  }
  const worker = {
    state: redundant ? 'redundant' : 'activated', scriptURL: origin + '/denial/sw.js',
    addEventListener() {}, removeEventListener() {},
    postMessage(value, ports) {
      check(value.other, other); dispatched.push(value.action);
      queueMicrotask(() => {
        if (timeout) clock.fire(12000);
        else ports[0].deliver({action: wrongGroup ? 'unrelated' : value.action,
          observations: (value.action === 'parallel' ? ['parallel-allow', 'parallel-deny'] : [value.action]).map(name => ({event: 'matrix-case', name}))});
      });
    }
  };
  const registration = {active: worker, scope: origin + '/denial/', async unregister() { ++unregisters; return true; }};
  const context = vm.createContext({
    URL, Array, MessageChannel: Channel, isSecureContext: true,
    location: new URL(origin + '/denial/' + (observer ? 'observer' : 'index') + '.html?other=' + other),
    setTimeout: clock.setTimeout, clearTimeout: clock.clearTimeout,
    chrome: {webview: {postMessage: value => messages.push(value)}},
    navigator: {serviceWorker: {
      async register(url, options) { check(url, '/denial/sw.js'); check(options.scope, '/denial/'); ++registrations; return registration; },
      async getRegistration(scope) { check(scope, '/denial/'); ++lookups; return registration; }
    }}
  });
  await vm.runInContext(pageScript, context);
  check(registrations, observer ? 0 : 1); check(lookups, observer ? 1 : 0);
  check(closedPorts, dispatched.length); check(clock.pending.size, 0);
  return {messages: JSON.parse(JSON.stringify(messages)), dispatched, unregisters};
}

async function main() {
  const run = worker((request, name) => {
    if (name.endsWith('-deny')) throw new TypeError('Failed to fetch');
    return response(request, name);
  });
  const all = [];
  for (const action of [...actions, 'teardown-deny', 'teardown-followup']) all.push(...await run(action));
  check(all.map(item => item.name), names);
  for (const item of all) {
    const denied = item.name.endsWith('-deny');
    check(item.fetch, denied ? 'rejected' : 'resolved'); check(item.body, denied ? 'none' : 'complete');
    check(item.status, denied ? 0 : 200); check(item.errorName, denied ? 'TypeError' : '');
    check(item.timedOut, false); check(item.aborted, false);
    check(item.timings.length, item.name.startsWith('parallel-') ? 2 : 1);
    for (const timing of item.timings) { check(timing.name, item.requestUrl); check(timing.type, 'fetch'); }
  }
  check(all.find(item => item.name === 'redirect-pass').redirected, true);
  check(all.find(item => item.name === 'redirect-pass').responseUrl, other + '/denial/network/redirect-pass-target');

  let allow, completed = [];
  const reverse = worker((request, name, clock, requests) => {
    check(requests.length, 2, 'both identical-URL requests must start before either result is released');
    if (name === 'parallel-allow') return new Promise(resolve => { allow = () => { completed.push(name); resolve(response(request, name)); }; });
    completed.push(name); queueMicrotask(() => allow()); throw new TypeError('Failed to fetch');
  });
  check((await reverse('parallel')).map(item => item.fetch), ['resolved', 'rejected']);
  check(completed, ['parallel-deny', 'parallel-allow']);
  const bodyFailure = (await worker((request, name) => response(request, name, {bodyFailure: true}))('post-deny'))[0];
  check(bodyFailure.fetch, 'resolved'); check(bodyFailure.body, 'rejected'); check(bodyFailure.errorName, 'TypeError');
  assert.ok(bodyFailure.bytes > 0); ++checks;
  const httpError = (await worker((request, name) => response(request, name, {status: 403}))('post-deny'))[0];
  check(httpError.fetch, 'resolved'); check(httpError.status, 403);
  const abort = (await worker((request, name, clock) => new Promise((resolve, reject) => {
    request.signal.addEventListener('abort', () => reject(new DOMException('deadline', 'AbortError')), {once: true});
    queueMicrotask(() => clock.fire(10000));
  }))('post-deny'))[0];
  check(abort.errorName, 'AbortError'); check(abort.timedOut, true); check(abort.aborted, true);
  const noTiming = (await worker(() => { throw new TypeError('Failed to fetch'); }, {noTiming: true})('post-deny'))[0];
  check(noTiming.timings, []);
  const changedTiming = (await worker(() => { throw new TypeError('Failed to fetch'); }, {timingName: 'gesher-denial-unrelated:blocked'})('post-deny'))[0];
  check(changedTiming.timings[0].name, 'gesher-denial-unrelated:blocked', 'observed identities must never be replaced with expected ones');

  const owner = await page(false), observer = await page(true);
  check(owner.dispatched, actions); check(owner.unregisters, 0, 'the owner leaves the worker alive for teardown observation');
  check(owner.messages.at(-1).event, 'matrix-main-complete');
  check(observer.dispatched, ['teardown-deny', 'teardown-followup']); check(observer.unregisters, 1);
  check(observer.messages[0].observer, true); check(observer.messages.at(-1).event, 'matrix-complete');
  check(observer.messages.at(-1).unregistered, true);
  for (const options of [{wrongGroup: true}, {timeout: true}, {redundant: true}]) {
    const bad = await page(true, options);
    check(bad.unregisters, 1); check(bad.messages.at(-1).event, 'matrix-failure');
    check(bad.messages.some(item => item.event === 'matrix-complete'), false);
  }
  console.log(`Worker denial scope scripts: ${checks} checks passed; isolated adapters, no WebView2 qualification`);
}
main().catch(error => { console.error(error); process.exitCode = 1; });
