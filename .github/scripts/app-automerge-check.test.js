'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const reporter = require('./app-automerge-check.js');
const {APP_ID, INSTALLATION_ID} = require('./app-qa-status.js');

function fixture() {
  const w = {calls: [], warnings: [], pr: {number: 7, state: 'open', auto_merge: null,
    head: {sha: 'a'.repeat(40)}, base: {ref: 'main'}}};
  const github = {rest: {pulls: {get: async () => ({data: w.pr})},
    checks: {create: async args => {if (w.error) throw w.error; w.calls.push(args);}}}};
  w.report = reporter({github, enabled: true, appId: APP_ID, installationId: INSTALLATION_ID,
    owner: 'o', repo: 'r', context: {runId: 123}, core: {warning: s => w.warnings.push(s)}});
  return w;
}

for (const outcome of ['success', 'neutral', 'failure']) {
  test('reports the real controller outcome: ' + outcome, async () => {
    const w = fixture(); await w.report(structuredClone(w.pr), outcome);
    assert.equal(w.calls[0].name, 'automerge');
    assert.equal(w.calls[0].head_sha, w.pr.head.sha);
    assert.equal(w.calls[0].conclusion, outcome);
    assert.equal(w.calls[0].details_url, 'https://github.com/o/r/actions/runs/123');
  });
}

test('does not publish on a changed head or wake inherited native waiting', async () => {
  const w = fixture(); const original = structuredClone(w.pr);
  w.pr.head.sha = 'b'.repeat(40); await w.report(original, 'success');
  w.pr = {...original, auto_merge: {merge_method: 'squash'}}; await w.report(original, 'neutral');
  assert.equal(w.calls.length, 0);
});

test('API failure only warns; it cannot replay or fail a protected merge', async () => {
  const w = fixture(); w.error = new Error('API unavailable');
  await w.report(w.pr, 'success');
  assert.equal(w.calls.length, 0);
  assert.equal(w.warnings.length, 1);
});

test('invalid outcome does not publish a check', async () => {
  const w = fixture(); await w.report(w.pr, 'skipped');
  assert.equal(w.calls.length, 0);
});
