#!/usr/bin/env python3
"""Exercise the exact probe worker's result detector, with explicit fetch adapters.

These are JavaScript detector controls, not Windows/WebView2 acceptance. The
real probe additionally requires a real worker, COM filter, faulty IStream and
independent origin counters. --negative-control deliberately conflates a body
read error with fetch rejection and must fail the body-error assertion.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


HARNESS = r"""
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const source = fs.readFileSync(process.argv[2], 'utf8');
const headers = {'X-Probe-Origin':'intercepted'};
let checks = 0;

async function observe(name, fetch, timeout = false) {
  const handlers = {}, results = [];
  let work;
  const context = vm.createContext({AbortController, TextDecoder, fetch,
    setTimeout: timeout ? callback => { queueMicrotask(callback); return 1; } : setTimeout,
    clearTimeout: timeout ? () => {} : clearTimeout,
    self:{addEventListener:(type, handler) => { handlers[type] = handler; }}});
  vm.runInContext(source, context);
  handlers.message({data:name, ports:[{postMessage:value => results.push(value)}],
                    waitUntil:promise => { work = promise; }});
  await work;
  assert.equal(results.length, 1, 'one completion per worker message');
  assert.equal(results[0].name, name);
  return results[0];
}

(async () => {
  for (const [name, body, status, origin] of [
      ['pass', 'origin:pass', 200, 'origin'], ['fulfill', 'supplied', 200, 'intercepted'],
      ['http403', 'denied', 403, 'intercepted']]) {
    const out = await observe(name, async (url, options) => {
      assert.equal(url, '/network/' + name); assert.equal(options.cache, 'no-store');
      assert.ok(options.signal instanceof AbortSignal);
      return new Response(body, {status, headers:{'X-Probe-Origin':origin}});
    });
    assert.equal(out.fetch, 'resolved', 'HTTP responses, including403, resolve fetch');
    assert.equal(out.body, 'complete'); assert.equal(out.status, status);
    assert.equal(out.bytes, body.length); assert.equal(out.text, body);
    assert.equal(out.originHeader, origin); assert.equal(out.timedOut, false);
    ++checks;
  }
  const before = await observe('first-read', async () => { throw new TypeError('injected network failure'); });
  assert.equal(before.fetch, 'rejected'); assert.equal(before.body, 'none');
  assert.equal(before.status, 0); assert.equal(before.bytes, 0); assert.equal(before.originHeader, '');
  assert.equal(before.timedOut, false); assert.match(before.error, /injected network failure/); ++checks;

  const firstBody = await observe('first-read', async () => new Response(new ReadableStream({
    start(controller) { controller.error(new Error('first body read failed')); }
  }), {headers}));
  assert.equal(firstBody.fetch, 'resolved', 'body failure preserves resolved fetch');
  assert.equal(firstBody.status, 200); assert.equal(firstBody.body, 'rejected');
  assert.equal(firstBody.bytes, 0); assert.equal(firstBody.originHeader, 'intercepted'); ++checks;

  let pulls = 0;
  const middle = await observe('mid-body', async () => new Response(new ReadableStream({
    pull(controller) {
      if (pulls++ === 0) controller.enqueue(new Uint8Array(65536).fill(65));
      else controller.error(new Error('mid-body read failed'));
    }
  }), {headers}));
  assert.equal(middle.fetch, 'resolved', 'mid-body failure preserves resolved fetch');
  assert.equal(middle.body, 'rejected'); assert.equal(middle.status, 200);
  assert.equal(middle.bytes, 65536); assert.equal(middle.text, 'A'.repeat(64)); ++checks;

  const shortBody = await observe('mid-body', async () => new Response('truncated', {headers}));
  assert.equal(shortBody.fetch, 'resolved'); assert.equal(shortBody.body, 'complete');
  assert.equal(shortBody.bytes, 9); ++checks;

  const expired = await observe('first-abort', async (url, options) => new Promise((resolve, reject) => {
    options.signal.addEventListener('abort', () => reject(new Error('test deadline')), {once:true});
  }), true);
  assert.equal(expired.fetch, 'rejected'); assert.equal(expired.timedOut, true,
    'deadline-induced abort is not a qualified backend network error'); ++checks;

  const oversized = await observe('mid-body', async () => new Response(new Uint8Array(262145), {headers}));
  assert.equal(oversized.fetch, 'resolved'); assert.equal(oversized.body, 'rejected');
  assert.equal(oversized.bytes, 262145); assert.match(oversized.error, /body limit/); ++checks;
  console.log('PASS worker-response detector: ' + checks + ' exact-script observations; host fetch adapters only');
})().catch(error => { console.error(error); process.exitCode = 1; });
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--node", default=os.environ.get("CODEX_PRIMARY_RUNTIME_NODE") or shutil.which("node"))
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if not args.node:
        parser.error("Node.js with Response/ReadableStream support is required")
    root = Path(__file__).resolve().parents[1]
    native = (root / "probes/webview/worker_response_server.cc").read_text()
    match = re.search(r'constexpr char kWorkerScript\[\] = R"JS\((.*?)\)JS";', native, re.S)
    if not match:
        raise SystemExit("worker script boundary missing; refusing a copied detector")
    script = match.group(1)
    if args.negative_control:
        old = "out.body = 'rejected';"
        assert script.count(old) == 1
        script = script.replace(old, "out.fetch = 'rejected'; out.body = 'none'; out.status = 0; out.bytes = 0;")
    with tempfile.TemporaryDirectory(prefix="gesher-worker-response-") as directory:
        temp = Path(directory)
        (temp / "worker.js").write_text(script)
        (temp / "control.cjs").write_text(HARNESS)
        subprocess.run([args.node, "--check", str(temp / "worker.js")], check=True)
        return subprocess.run([args.node, str(temp / "control.cjs"), str(temp / "worker.js")], timeout=15).returncode


if __name__ == "__main__":
    raise SystemExit(main())
