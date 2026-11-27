'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const publisher = require('./app-qa-status.js');
const SHA = 'a'.repeat(40);
function fixture() {
  const receipt = {id: 42, context: publisher.APP_STATUS, state: 'success',
    url: `https://api.github.com/repos/o/r/statuses/${SHA}`,
    creator: {id: 358279050, login: 'gesher-runtime-automation[bot]', type: 'Bot'}};
  const w = {receipt, full: [receipt], current: receipt, reads: [], afterFull: () => {}};
  const readGithub = {rest: {repos: {
    getCombinedStatusForRef: async args => {
      w.reads.push(['combined', args]);
      const {creator, ...row} = w.current;
      return {data: {sha: SHA, statuses: [row]}};
    },
    listCommitStatusesForRef: async args => {
      w.reads.push(['full', args]);
      const data = w.full.slice((args.page - 1) * args.per_page, args.page * args.per_page);
      w.afterFull();
      return {data};
    },
  }}};
  const github = {rest: {repos: {createCommitStatus: async () => assert.fail('legacy write')}}};
  const appGithub = {rest: {repos: {createCommitStatus: async () => ({data: receipt})}}};
  w.gate = publisher({github, readGithub, appGithub, owner: 'o', repo: 'r', enabled: true,
    appId: publisher.APP_ID, installationId: publisher.INSTALLATION_ID});
  return w;
}
test('App readback authenticates the exact current ID when combined status omits creator', async () => {
  const w = fixture();
  assert.equal(await w.gate.prepareSuccess(SHA), w.receipt);
  assert.deepEqual(w.reads.map(([kind]) => kind), ['combined', 'full', 'combined']);
  assert.ok(w.reads.every(([, args]) => args.ref === SHA));
});
for (const [name, change] of Object.entries({
  missing: w => { w.full = []; },
  'different ID': w => { w.full = [{...w.receipt, id: 41}]; },
  'foreign writer': w => { w.full = [{...w.receipt, creator: {...w.receipt.creator, id: 41898282}}]; },
  'wrong login': w => { w.full = [{...w.receipt, creator: {...w.receipt.creator, login: 'other[bot]'}}]; },
  'non-bot writer': w => { w.full = [{...w.receipt, creator: {...w.receipt.creator, type: 'User'}}]; },
  'missing creator': w => { w.full = [{...w.receipt, creator: undefined}]; },
  'wrong repository': w => { w.full = [{...w.receipt, url: `https://api.github.com/repos/other/r/statuses/${SHA}`}]; },
  'wrong head': w => { w.full = [{...w.receipt, url: 'https://api.github.com/repos/o/r/statuses/' + 'b'.repeat(40)}]; },
  'wrong context': w => { w.full = [{...w.receipt, context: 'unrelated'}]; },
  'wrong state': w => { w.full = [{...w.receipt, state: 'pending'}]; },
  'replacement during provenance read': w => { w.afterFull = () => { w.current = {...w.receipt, id: 43}; }; },
})) test('App provenance read fails closed for ' + name, async () => {
  const w = fixture(); change(w);
  await assert.rejects(w.gate.prepareSuccess(SHA), /App QA status is missing, replaced/);
});
test('App provenance scans only through the exact receipt, without requiring old history', async () => {
  const w = fixture();
  w.full = [...Array.from({length: 100}, (_, i) => ({id: 1000 + i, context: 'other'})),
    w.receipt, ...Array.from({length: 1200}, (_, i) => ({id: i + 2000, context: 'older'}))];
  await w.gate.prepareSuccess(SHA);
  assert.deepEqual(w.reads.filter(([kind]) => kind === 'full').map(([, args]) => args.page), [1, 2]);
});
test('App provenance scan is bounded and cannot authorize an absent receipt from partial history', async () => {
  const w = fixture();
  w.full = Array.from({length: 1100}, (_, i) => ({id: 1000 + i, context: 'other'}));
  await assert.rejects(w.gate.prepareSuccess(SHA), /App QA status is missing, replaced/);
  assert.equal(w.reads.filter(([kind]) => kind === 'full').length, 10);
});
