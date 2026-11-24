// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Execute the exact embedded discovery script with isolated Mojo adapters.
// These verify completion/error handling, not browser inventory or persistence.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const script = fs.readFileSync(process.argv[2], 'utf8');
let checks = 0;

async function execute({method, missingModule = false, missingInterface = false,
                        hostname = 'indexeddb-internals'} = {}) {
  const context = vm.createContext({location: {
    protocol: 'edge:', hostname, href: 'edge://' + hostname + '/'
  }});
  const module = new vm.SyntheticModule(['IdbInternalsHandler'], function() {
    this.setExport('IdbInternalsHandler', missingInterface ? {} : {
      getRemote: () => ({getAllBucketsAcrossAllStorageKeys: method})
    });
  }, {context});
  await module.link(() => { throw Error('Unexpected synthetic module import'); });
  await module.evaluate();
  return new vm.Script(script, {
    importModuleDynamically: async specifier => {
      assert.equal(specifier, './indexed_db_internals.mojom-webui.js');
      if (missingModule) throw Error('Built-in module unavailable');
      return module;
    }
  }).runInContext(context);
}

async function main() {
  let complete, calls = 0, settled = false;
  const pending = new Promise(resolve => { complete = resolve; });
  const observed = execute({method: () => { ++calls; return pending; }}).then(
      value => { settled = true; return {value}; },
      error => { settled = true; return {error}; });
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(calls, 1, 'the real discovery method must be called once'); ++checks;
  assert.equal(settled, false, 'discovery completion must be awaited'); ++checks;
  complete({error: null, partitions: [{}]});
  const completed = await observed;
  assert.equal(completed.error, undefined);
  assert.equal(completed.value.initialized, true);
  assert.equal(completed.value.partitions, '1'); ++checks;

  for (const result of [null, {}, {error: 'disk discovery failed', partitions: []},
                        {error: null, partitions: null}, {error: null, partitions: []},
                        {error: null, partitions: Array(65).fill({})}]) {
    await assert.rejects(execute({method: async () => result}), /IndexedDB discovery/); ++checks;
  }
  await assert.rejects(execute({method: async () => { throw Error('native reply rejected'); }}),
                       /native reply rejected/); ++checks;
  for (const options of [{missingModule: true}, {missingInterface: true}, {},
                         {hostname: 'unrelated'}]) {
    const unavailable = await execute(options);
    assert.equal(typeof unavailable.unavailable, 'string');
    assert.equal(unavailable.initialized, undefined); ++checks;
  }
  console.log(`IndexedDB discovery barrier: ${checks} checks passed`);
}
main().catch(error => { console.error(error); process.exitCode = 1; });
