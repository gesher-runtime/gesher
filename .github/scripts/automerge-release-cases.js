'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const {world, HEAD, OTHER, APP_BOT} = require('./automerge-fixture.js');
const app = require('./app-qa-status.js');
const {RELEASE_STATE, RELEASE_RETRY} = require('./label-automerge.js');
const ACTIONS_BOT = {id: 41898282, login: 'github-actions[bot]', type: 'Bot'};
const last = (w, context) => w.statuses.filter(s => s.context === context).sort((a, b) => b.id - a.id)[0];
function setup(contexts = [app.APP_STATUS], extra = {}) {
  const w = world({repositoryId: 1426153186, appQaStatus: true, appId: app.APP_ID,
    appInstallationId: app.INSTALLATION_ID, nativePolicy: {contexts},
    rules: [{type: 'required_status_checks', parameters: {required_status_checks:
      contexts.map(context => ({context, integration_id: context === app.APP_STATUS ? app.APP_ID : 15368}))}}],
    ...extra});
  w.useApp();
  return w;
}
function seed(w, contexts) {
  for (const context of contexts) w.statuses.push({id: w.nextStatusId++, context, state: 'success',
    creator: context === app.APP_STATUS ? APP_BOT : ACTIONS_BOT,
    url: `https://api.github.com/repos/o/r/statuses/${HEAD}`});
}
const retries = w => w.did('app-dispatch').map(([, args]) => args.client_payload.release_retry)
  .filter(retry => retry !== undefined);
function noRelease(w) {
  assert.equal(w.nativeMerges.length, 0);
  assert.equal(w.did('nativeMerge').length, 0);
  assert.equal(w.did('nativeWait').length, 0);
}

for (const contexts of [[app.STATUS], [app.APP_STATUS], [app.STATUS, app.APP_STATUS]]) {
  test(`ready auto-merge enable rejects CLEAN under ${contexts.join(',')}`, async () => {
    const w = setup(contexts); seed(w, contexts);
    await assert.rejects(w.prGithub.graphql('mutation enablePullRequestAutoMerge', {id: 'PR_7', head: HEAD}), /clean status/);
    assert.equal(w.nativeMerges.length, 0);
    assert.equal(w.nativeAutoMethod, null);
  });
  for (const state of ['CLEAN', 'HAS_HOOKS', 'UNSTABLE']) {
    test(`protected ${state} release under ${contexts.join(',')} retains exact-head policy`, async () => {
      const w = setup(contexts, {nativeStateOverride: {mergeStateStatus: state}});
      await w.ciDone();
      assert.equal(w.pr.state, 'closed');
      assert.equal(w.nativeMerges.length, 1);
      assert.deepEqual(w.did('nativeMerge')[0][1], {id: 'PR_7', head: HEAD});
      assert.equal(w.nativeMerges[0].stage, 'merge');
      assert.equal(w.nativeMerges[0].label, true);
      assert.equal(w.nativeMerges[0].permission, 'write');
      assert.equal(w.did('nativeWait').length, 0);
      assert.equal(w.did('merge').length, 0, 'no REST/admin fallback');
      assert.equal(w.calls.at(-1)[0], 'nativeMerge');
    });
  }
  for (const state of ['BLOCKED', 'BEHIND']) {
    test(`protected ${state} stays disarmed without bypass under ${contexts.join(',')}`, async () => {
      const w = setup(contexts, {nativePolicy: {contexts, blocked: true},
        nativeStateOverride: {mergeStateStatus: state}});
      await w.ciDone();
      assert.equal(w.pr.state, 'open');
      assert.equal(w.nativeMerges.length, 0);
      assert.equal(w.did('nativeMerge').length, 0);
      assert.equal(w.did('nativeWait').length, 0);
      assert.equal(w.nativeAutoMethod, null);
      assert.equal(last(w, app.APP_STATUS).state, 'success');
    });
  }
}

for (const [name, override] of Object.entries({
  unknown: {mergeStateStatus: 'UNKNOWN'}, conflict: {mergeStateStatus: 'DIRTY'},
  draft: {isDraft: true}, closed: {state: 'CLOSED'}, changed: {headRefOid: OTHER},
  retargeted: {baseRefName: 'other'}, identity: {id: 'PR_OTHER'},
  ambiguous: {mergeable: 'UNKNOWN'}, unmergeable: {mergeable: 'CONFLICTING'},
})) {
  test(`native release state rejects ${name} without fallback`, async () => {
    const w = setup(undefined, {nativeStateOverride: override});
    await assert.rejects(w.ciDone(), /Native merge state/);
    noRelease(w);
    assert.equal(last(w, app.APP_STATUS).state, 'failure');
    assert.equal(last(w, app.STATUS).state, 'failure');
  });
}

for (const [name, revoke] of Object.entries({
  label: w => {w.pr.labels = [];}, permission: w => {w.permission = 'read';},
  receipt: w => {w.reviews[0].state = 'DISMISSED';},
  'CI rerun': w => {Object.assign(w.runs[0], {run_attempt: 2, status: 'in_progress', conclusion: null});},
  'CI failure': w => {w.runs[0].conclusion = 'failure';},
  registration: w => {w.workflowOverride = {state: 'disabled_manually'};},
})) {
  test(`${name} change during native state read is revalidated before release`, async () => {
    const w = setup(); w.hooks.nativeState = () => revoke(w);
    await w.ciDone();
    noRelease(w);
    assert.notEqual(last(w, app.APP_STATUS)?.state, 'success');
  });
}

for (const [name, revoke] of Object.entries({
  head: w => {w.pr.head.sha = OTHER;},
  'required checks': w => {w.checks[0].conclusion = 'failure';},
  'required review': w => {for (const review of w.reviews) review.state = 'DISMISSED';},
  'native protection': w => {w.nativePolicy.blocked = true;},
})) {
  test(`server policy still rejects changed ${name} at the release boundary`, async () => {
    const w = setup(); w.hooks.nativeMerge = () => revoke(w);
    await assert.rejects(w.ciDone(), /native merge head changed|repository policy blocks/);
    assert.equal(w.pr.state, 'open');
    assert.equal(w.nativeMerges.length, 0);
    assert.equal(w.did('nativeMerge').length, 1);
    assert.equal(w.did('nativeWait').length, 0);
    assert.equal(w.did('merge').length, 0);
  });
}

test('a stale blocked-state read defers release until a fresh ordinary reconciliation', async () => {
  const w = setup(undefined, {nativeStateOverride: {mergeStateStatus: 'BLOCKED'}});
  await w.ciDone();
  assert.equal(w.pr.state, 'open');
  assert.equal(w.did('nativeWait').length, 0);
  assert.equal(w.did('nativeMerge').length, 0);
  assert.equal(w.nativeAutoMethod, null);
  assert.equal(last(w, app.APP_STATUS).state, 'success');
  delete w.nativeStateOverride;
  await w.sweep();
  assert.equal(w.nativeMerges.length, 1);
  assert.equal(w.nativeMerges[0].stage, 'merge');
});

test('a briefly stale blocked state after the gate write is re-read and released in the same pass', async () => {
  const w = setup(undefined, {nativeStateOverride: {mergeStateStatus: 'BLOCKED'}});
  let reads = 0;
  w.hooks.nativeState = () => { if (++reads === 3) delete w.nativeStateOverride; };
  await w.ciDone();
  assert.equal(w.pr.state, 'closed');
  assert.equal(w.nativeMerges.length, 1);
  assert.equal(w.nativeMerges[0].stage, 'merge');
  assert.equal(w.did('nativeWait').length, 0);
  assert.equal(w.did('nativeState').length, 3);
});

test('a persistently blocked state is re-read a bounded number of times before deferring', async () => {
  const w = setup(undefined, {nativeStateOverride: {mergeStateStatus: 'BLOCKED'}});
  await w.fire('workflow_run', {workflow_run: {event: 'pull_request', head_sha: HEAD, head_repository: {id: 10}}});
  await w.fire('repository_dispatch', w.dispatches.shift());
  assert.equal(w.pr.state, 'open');
  assert.equal(w.did('nativeMerge').length, 0);
  assert.equal(w.did('nativeState').length, RELEASE_STATE.attempts);
  assert.deepEqual(w.dispatches.map(d => d.client_payload),
    [{pull_request_number: 7, head_sha: HEAD, release_retry: 1}]);
});

for (const mode of [{name: 'legacy', enabled: false, contexts: [app.STATUS]},
  {name: 'App required', enabled: true, contexts: [app.APP_STATUS]}]) {
  test(`${mode.name}: a block that outlasts one pass is released by a queued exact-head retry`, async () => {
    const w = setup(mode.contexts, {appQaStatus: mode.enabled,
      nativeStateOverride: {mergeStateStatus: 'BLOCKED'}});
    let reads = 0;
    w.hooks.nativeState = () => { if (++reads === RELEASE_STATE.attempts * 2 + 1) delete w.nativeStateOverride; };
    await w.ciDone();
    assert.equal(w.pr.state, 'closed');
    assert.equal(w.nativeMerges.length, 1);
    assert.equal(w.nativeMerges[0].stage, 'merge');
    assert.equal(w.did('nativeWait').length, 0);
    assert.deepEqual(retries(w), [1, 2]);
  });

  test(`${mode.name}: a persistent block queues a bounded number of retries and logs it`, async () => {
    const w = setup(mode.contexts, {appQaStatus: mode.enabled,
      nativeStateOverride: {mergeStateStatus: 'BLOCKED'}});
    await w.ciDone();
    assert.equal(w.pr.state, 'open');
    noRelease(w);
    assert.equal(retries(w).length, RELEASE_RETRY.max);
    assert.equal(w.did('nativeState').length, RELEASE_STATE.attempts * (RELEASE_RETRY.max + 1));
    assert.ok(w.warnings.some(s => s.includes('BLOCKED') && s.includes('queued release retries')));
  });
}

test('a queued release retry is bound to its observed head', async () => {
  const w = setup(undefined, {nativeStateOverride: {mergeStateStatus: 'BLOCKED'}});
  await w.fire('workflow_run', {workflow_run: {event: 'pull_request', head_sha: HEAD, head_repository: {id: 10}}});
  await w.fire('repository_dispatch', w.dispatches.shift());
  w.pr.head.sha = OTHER;
  delete w.nativeStateOverride;
  const before = w.calls.length;
  await w.drainDispatches();
  assert.deepEqual(w.calls.slice(before).map(([name]) => name), ['getPR']);
  noRelease(w);
});

test('a failed retry dispatch is logged without revoking the green gate', async () => {
  const w = setup([app.STATUS], {appQaStatus: false, nativeStateOverride: {mergeStateStatus: 'BLOCKED'},
    dispatchError: new Error('dispatch denied')});
  await w.ciDone();
  assert.equal(w.pr.state, 'open');
  noRelease(w);
  assert.equal(last(w, app.STATUS).state, 'success');
  assert.ok(w.warnings.some(s => s.includes('release retry could not be queued')));
});

for (const retry of [0, -1, '1', 1.5, 9]) {
  test(`an invalid release retry count cannot perform writes: ${JSON.stringify(retry)}`, async () => {
    for (const enabled of [false, true]) {
      const w = setup(undefined, {appQaStatus: enabled});
      await assert.rejects(w.fire('repository_dispatch', {action: 'gesher-automerge-reconcile',
        client_payload: {pull_request_number: 7, head_sha: HEAD, release_retry: retry}}),
      /Invalid queued PR reconciliation/);
      assert.deepEqual(w.calls, []);
    }
  });
}

test('legacy mode still ignores ordinary queued reconciliations', async () => {
  const w = setup([app.STATUS], {appQaStatus: false});
  await w.fire('repository_dispatch', {action: 'gesher-automerge-reconcile',
    client_payload: {pull_request_number: 7, head_sha: HEAD}});
  assert.deepEqual(w.calls, []);
});

test('an ambiguous completed merge is read back without cleanup writes or another release', async () => {
  const w = setup(undefined, {nativeMergeResponseError: new Error('response lost')});
  await assert.rejects(w.ciDone(), /response lost/);
  assert.equal(w.pr.state, 'closed');
  assert.equal(w.nativeMerges.length, 1);
  assert.equal(w.did('nativeMerge').length, 1);
  assert.equal(w.did('nativeWait').length, 0);
  const at = w.calls.findIndex(([name]) => name === 'nativeMerge');
  assert.deepEqual(w.calls.slice(at + 1).map(([name]) => name), ['getPR']);
  assert.equal(last(w, app.APP_STATUS).state, 'success');
});

test('a blocked target retires old waiting without issuing another waiting mutation', async () => {
  const w = setup(undefined, {nativePolicy: {contexts: [app.APP_STATUS], blocked: true},
    nativeAutoMethod: 'SQUASH',
    nativeWaitResponseError: new Error('waiting response lost')});
  await w.ciDone();
  assert.equal(w.did('nativeWait').length, 0);
  assert.equal(w.did('nativeMerge').length, 0);
  assert.equal(w.did('nativeCancel').length, 1);
  assert.equal(w.nativeAutoMethod, null);
  const at = w.calls.findIndex(([name]) => name === 'nativeCancel');
  assert.equal(w.calls[at + 1][0], 'getPR');
  assert.equal(last(w, app.APP_STATUS).state, 'success');
});

test('an unreadable ambiguous release state stops writes until later reconciliation', async () => {
  const w = setup();
  w.hooks.nativeMerge = () => {
    w.hooks.getPR = () => {throw new Error('state unavailable');};
    throw new Error('release response unavailable');
  };
  await assert.rejects(w.ciDone(), /release response unavailable/);
  assert.equal(w.did('nativeMerge').length, 1);
  assert.equal(w.did('nativeWait').length, 0);
  const at = w.calls.findIndex(([name]) => name === 'nativeMerge');
  assert.deepEqual(w.calls.slice(at + 1).map(([name]) => name), ['getPR']);
  assert.ok(w.warnings.some(s => s.includes('cleanup deferred')));
});

for (const contexts of [[app.APP_STATUS], [app.APP_STATUS, app.STATUS]]) {
  for (const reason of ['denied', 'readback']) {
    test(`shared App failure (${reason}) cannot trigger an old armed success under ${contexts.join(',')}`, async () => {
      const w = setup(contexts, {nativeAutoMethod: 'SQUASH'}); seed(w, contexts); w.pr.labels = [];
      w.hooks.nativeCancel = () => {throw new Error('cancellation denied');};
      if (reason === 'denied') w.statusErrors = {'app-status': new Error('status denied')};
      else w.statusReadbackHead = OTHER;
      await assert.rejects(w.fire('repository_dispatch', {action: 'gesher-automerge-reconcile',
        client_payload: {pull_request_number: 7, head_sha: HEAD}}), /cancellation denied/);
      assert.equal(w.pr.state, 'open');
      assert.equal(w.nativeMerges.length, 0);
      assert.equal(w.did('status').length, 0, 'no unrelated Actions mutation');
      assert.equal(w.did('nativeMerge').length, 0);
      assert.equal(w.did('nativeWait').length, 0);
      assert.ok(w.warnings.some(s => s.includes('QA revocation unconfirmed')));
    });
  }
}

for (const condition of ['legacy-only', 'wrong integration', 'unreadable rules']) {
  test(`unconfirmed disarming with ${condition} cannot write an unrequired App shadow`, async () => {
    const w = setup([app.STATUS], {nativeAutoMethod: 'SQUASH'}); seed(w, [app.STATUS]); w.pr.labels = [];
    if (condition === 'wrong integration') w.rules[0].parameters.required_status_checks.push({
      context: app.APP_STATUS, integration_id: 15368});
    if (condition === 'unreadable rules') w.rulesError = new Error('rules unavailable');
    w.hooks.nativeCancel = () => {throw new Error('cancellation denied');};
    await assert.rejects(w.ciDone(), /cancellation denied/);
    assert.equal(w.pr.state, 'open');
    assert.equal(w.nativeMerges.length, 0);
    assert.equal(w.did('app-status').length, 0);
    assert.equal(w.did('status').length, 0);
  });
}

test('an ambiguous successful cancellation is read back before safe cleanup', async () => {
  const w = setup([app.STATUS], {nativeAutoMethod: 'SQUASH'}); seed(w, [app.STATUS]); w.pr.labels = [];
  w.hooks.nativeCancel = () => {w.nativeAutoMethod = null; throw new Error('cancel response lost');};
  await assert.rejects(w.ciDone(), /cancel response lost/);
  assert.equal(w.did('nativeCancel').length, 1, 'no repeat after readback confirms cancellation');
  assert.equal(w.nativeMerges.length, 0);
  assert.equal(w.nativeAutoMethod, null);
  assert.equal(last(w, app.STATUS).state, 'failure');
  assert.equal(last(w, app.APP_STATUS).state, 'failure');
});
