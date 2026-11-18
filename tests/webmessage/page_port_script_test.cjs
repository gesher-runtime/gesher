/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const {MessageChannel} = require('node:worker_threads');
const {webcrypto} = require('node:crypto');
const script = fs.readFileSync(process.argv[2], 'utf8');
let checks = 0;
const allPorts = new Set();
const pages = [];
function check(value, reason) { checks++; assert.ok(value, reason); }
function channel() {
  const value = new MessageChannel();
  allPorts.add(value.port1); allPorts.add(value.port2);
  return value;
}
async function waitFor(condition, reason) {
  const deadline = Date.now() + 2000;
  while (!condition()) {
    if (Date.now() >= deadline) throw new Error('Timed out: ' + reason);
    await new Promise(resolve => setTimeout(resolve, 1));
  }
}
function page() {
  const window = new EventTarget();
  const messages = [], sent = [], listeners = [];
  window.top = window;
  const value = {window, messages, sent, onSend: null};
  const chrome = {webview: {
    postMessage(message) {
      sent.push(structuredClone(message));
      if (value.onSend) value.onSend(message);
    },
    addEventListener(type, listener) { assert.equal(type, 'message'); listeners.push(listener); }
  }};
  window.chrome = chrome;
  window.addEventListener('message', e => {
    messages.push(e);
    e.ports.forEach(p => allPorts.add(p));
  });
  value.deliver = message => listeners.forEach(listener => listener({data: message}));
  vm.runInNewContext(script, {window, chrome, crypto: webcrypto, location: {origin: 'https://example.test'},
    MessageChannel, MessageEvent, ArrayBuffer, Uint8Array, Uint32Array, btoa, atob}, {timeout: 1000});
  pages.push(value);
  return value;
}
function carrier(p, id) {
  p.deliver({gesher: 'post', origin: 'https://example.test', data: 'initial', ports: [id]});
  check(p.messages.length === 1 && p.messages[0].data === 'initial', 'host window message delivered');
  check(p.messages[0].ports.length === 1, 'host transfers a real page MessagePort');
  return p.messages[0].ports[0];
}

async function main() {
  const p = page(), root = carrier(p, 'java-carrier');
  const child = channel();
  child.port1.postMessage('queued before transfer');
  root.postMessage('take child', [child.port2]);
  await waitFor(() => p.sent.length >= 1, 'page transfer');
  const transfer = p.sent[0];
  check(transfer.gesherPort === 'java-carrier' && transfer.data === 'take child', 'parent payload reaches bridge');
  check(Array.isArray(transfer.ports) && transfer.ports.length === 1, 'e.ports is carried to Java');
  const childId = transfer.ports[0];
  check(/^page:[0-9a-f]{32}:[1-9][0-9]{0,15}$/.test(childId), 'fresh page endpoint has a document-local id');
  await waitFor(() => p.sent.length >= 2, 'child queue flush');
  check(p.sent[1].gesherPort === childId && p.sent[1].data === 'queued before transfer', 'parent publication precedes already-queued child data');
  const replies = [];
  child.port1.onmessage = e => { replies.push(e); e.ports.forEach(p => allPorts.add(p)); };
  p.deliver({gesher: 'port', id: childId, data: 'reply from Java', ports: []});
  await waitFor(() => replies.length === 1, 'Java reply');
  check(replies[0].data === 'reply from Java', 'received endpoint is bidirectional');

  const grandchild = channel();
  child.port1.postMessage(new Uint8Array([0, 255, 128]).buffer, [grandchild.port2]);
  await waitFor(() => p.sent.length >= 3, 'nested binary transfer');
  const nested = p.sent[2], nestedId = nested.ports[0];
  check(nested.binary === true && nested.data === 'AP+A', 'native ArrayBuffer bytes survive encoding');
  check(nested.gesherPort === childId && nested.ports.length === 1 && nestedId !== childId, 'nested transferred endpoint is retained');
  const binaryReplies = [];
  grandchild.port1.onmessage = e => binaryReplies.push(e);
  p.deliver({gesher: 'port', id: nestedId, data: 'BAUG', binary: true, ports: []});
  await waitFor(() => binaryReplies.length === 1, 'binary reply');
  check(Array.from(new Uint8Array(binaryReplies[0].data)).join(',') === '4,5,6', 'Java binary payload reaches native child peer');

  p.deliver({gesher: 'port', id: childId, data: 'Java nested transfer', ports: ['java-nested']});
  await waitFor(() => replies.length === 2, 'Java nested endpoint');
  check(replies[1].ports.length === 1, 'Java-to-page nested transfer stays native');
  replies[1].ports[0].postMessage('nested reply');
  await waitFor(() => p.sent.some(m => m.gesherPort === 'java-nested'), 'native nested reply');
  check(p.sent.find(m => m.gesherPort === 'java-nested').data === 'nested reply', 'Java-to-page path remains bidirectional');

  const pair = channel();
  root.postMessage('both ends', [pair.port1, pair.port2]);
  await waitFor(() => p.sent.some(m => m.data === 'both ends'), 'both endpoints');
  const both = p.sent.find(m => m.data === 'both ends').ports;
  check(both.length === 2 && both[0] !== both[1], 'both endpoints may be transferred in one message');
  p.deliver({gesher: 'port', id: both[0], data: 'entangled after transfer', ports: []});
  await waitFor(() => p.sent.some(m => m.gesherPort === both[1]), 'pair entanglement');
  check(p.sent.find(m => m.gesherPort === both[1]).data === 'entangled after transfer', 'native entanglement survives dual transfer');

  const orphan = channel();
  let orphanId;
  p.onSend = message => {
    if (message.data === 'rejected by Java') {
      orphanId = message.ports[0];
      p.deliver({gesher: 'close', id: orphanId});
    }
  };
  root.postMessage('rejected by Java', [orphan.port2]);
  await waitFor(() => orphanId !== undefined, 'reentrant rejection');
  check(!Object.hasOwn(p.window.__gesherPorts, orphanId), 'rejected transfer is not restarted after its parent publication');
  p.onSend = null;

  p.deliver({gesher: 'close', id: nestedId});
  check(!Object.hasOwn(p.window.__gesherPorts, nestedId), 'app close removes the page transport handle');
  p.deliver({gesher: 'close', id: nestedId});
  check(!Object.hasOwn(p.window.__gesherPorts, nestedId), 'close is idempotent');

  const next = page(), nextRoot = carrier(next, 'next-document'), nextChild = channel();
  nextRoot.postMessage('new document', [nextChild.port2]);
  await waitFor(() => next.sent.length > 0, 'new document transfer');
  const nextId = next.sent[0].ports[0];
  check(nextId !== childId, 'document replacement uses a different transfer-id namespace');
  next.deliver({gesher: 'close', id: childId});
  check(Object.hasOwn(next.window.__gesherPorts, nextId), 'late old-document cleanup cannot close a new endpoint');
  console.log(`Page port script: ${checks} checks, real MessageChannel transfer/queues/binary; window/WebView2 adapter`);
}

main().catch(error => { console.error(error); process.exitCode = 1; }).finally(() => {
  for (const p of pages) {
    for (const port of Object.values(p.window.__gesherPorts || {})) {
      port.onmessage = null; port.close();
    }
  }
  for (const port of allPorts) { port.onmessage = null; port.close(); }
});
