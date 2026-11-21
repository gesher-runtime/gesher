'use strict';
// Policy model only: no live GitHub mutation or protection change.
const test = require('node:test');
const assert = require('node:assert/strict');
const controller = require('./label-automerge.js');
const app = require('./app-qa-status.js');
const {world, HEAD, OTHER, BASE, T, APP_BOT} = require('./automerge-fixture.js');
const ACTIONS_BOT = {id: 41898282, login: 'github-actions[bot]', type: 'Bot'};
const modes = [
  {name: 'legacy', enabled: false, contexts: [app.STATUS]},
  {name: 'App shadow', enabled: true, contexts: [app.STATUS]},
  {name: 'App required', enabled: true, contexts: [app.APP_STATUS]},
  {name: 'dual required', enabled: true, contexts: [app.STATUS, app.APP_STATUS]},
];

function setup(mode, extra = {}) {
  const w = world({repositoryId: 1426153186, appQaStatus: mode.enabled,
    appId: app.APP_ID, appInstallationId: app.INSTALLATION_ID,
    nativePolicy: {contexts: mode.contexts, dismissStaleReviews: false},
    rules: [{type: 'required_status_checks', parameters: {required_status_checks:
      mode.contexts.map(context => ({context, integration_id: context === app.APP_STATUS ? app.APP_ID : 15368}))}}],
    ...extra});
  w.useApp();
  return w;
}
function greenFor(w, sha, contexts) {
  for (const context of contexts) w.statuses.unshift({id: w.nextStatusId++, context, state: 'success',
    creator: context === app.APP_STATUS ? APP_BOT : ACTIONS_BOT,
    url: `https://api.github.com/repos/o/r/statuses/${sha}`});
}
function moveToGreenHead(w, mode) {
  w.pr.head.sha = OTHER;
  w.pr.updated_at = T(45);
  for (const run of w.runs) Object.assign(run, {head_sha: OTHER, status: 'completed', conclusion: 'success'});
  for (const check of w.checks) Object.assign(check, {status: 'completed', conclusion: 'success'});
  greenFor(w, OTHER, mode.contexts);
  w.nativePolicy.blocked = false;
}

// Calibrate the model against both stale-review settings. A correct model must
// expose the old request's race rather than silently require current-head reviews.
for (const dismissStaleReviews of [false, true]) {
  test(`native policy model counts prior reviews only when dismissal is off (${dismissStaleReviews})`, async () => {
    const mode = modes[0];
    const w = setup(mode, {nativeAutoMethod: 'SQUASH',
      nativePolicy: {contexts: mode.contexts, dismissStaleReviews}});
    moveToGreenHead(w, mode);
    assert.equal(await controller.approved(w.github, 'o', 'r', w.pr), false);
    await w.evaluateNative();
    assert.equal(w.nativeMerges.length, dismissStaleReviews ? 0 : 1);
  });
}

for (const mode of modes) {
  for (const held of ['CI', 'native rule']) {
    test(`${mode.name}: previously-green head before synchronize cannot consume old authorization while waiting for ${held}`, async () => {
      const w = setup(mode);
      if (held === 'CI') {
        Object.assign(w.runs[0], {status: 'in_progress', conclusion: null});
        Object.assign(w.checks[0], {status: 'in_progress', conclusion: null});
      } else {
        w.nativePolicy.blocked = true;
      }
      await w.ciDone();
      assert.equal(w.pr.state, 'open');
      assert.equal(w.reviews.length, 1, 'the original approval is retained');
      moveToGreenHead(w, mode);
      assert.equal(await controller.approved(w.github, 'o', 'r', w.pr), false);
      // Native policy evaluates a write-capable collaborator's new target before
      // the asynchronous synchronize controller has an opportunity to clean up.
      await w.evaluateNative();
      assert.equal(w.nativeMerges.length, 0, 'new head must remain unmerged before synchronize');
      assert.equal(w.pr.state, 'open');
      assert.equal(w.nativeAutoMethod, null);
      await w.pushed();
      assert.deepEqual(w.pr.labels, []);
      assert.equal(w.reviews[0].state, 'DISMISSED');
      assert.equal(w.nativeMerges.length, 0);
      assert.equal(w.did('nativeWait').length, 0);
    });
  }

  test(`${mode.name}: blocked rule later clears through fresh exact-head recovery`, async () => {
    const w = setup(mode);
    w.nativePolicy.blocked = true;
    await w.ciDone();
    assert.equal(w.nativeAutoMethod, null);
    assert.equal(w.nativeMerges.length, 0);
    const approvals = w.reviews.length;
    // Changing main's SHA alone retains the same source-head authorization.
    w.pr.base.sha = OTHER;
    assert.notEqual(w.pr.base.sha, BASE);
    w.nativePolicy.blocked = false;
    await w.sweep();
    assert.equal(w.pr.state, 'closed');
    assert.equal(w.nativeMerges.length, 1);
    assert.deepEqual(w.did('nativeMerge')[0][1], {id: 'PR_7', head: HEAD});
    assert.equal(w.nativeMerges[0].stage, 'merge');
    assert.equal(w.reviews.length, approvals);
    assert.equal(w.did('dispatch').length, 0, 'no second full CI run');
    assert.equal(w.did('nativeWait').length, 0);
  });

  for (const [name, revoke] of Object.entries({
    label: w => {w.pr.labels = [];},
    permission: w => {w.permission = 'read';},
    receipt: w => {w.reviews[0].state = 'DISMISSED';},
    'CI rerun': w => {Object.assign(w.runs[0], {run_attempt: 2, status: 'in_progress', conclusion: null});},
    registration: w => {w.workflowOverride = {state: 'disabled_manually'};},
  })) {
    test(`${mode.name}: ${name} revoked at the final state read cannot release`, async () => {
      const w = setup(mode);
      w.hooks.nativeState = () => revoke(w);
      await w.ciDone();
      assert.equal(w.nativeMerges.length, 0);
      assert.equal(w.nativeAutoMethod, null);
      assert.equal(w.did('nativeMerge').length, 0);
      assert.equal(w.did('nativeWait').length, 0);
    });
  }

  test(`${mode.name}: atomic expectedHeadOid rejects a changed head at release`, async () => {
    const w = setup(mode);
    w.hooks.nativeMerge = () => moveToGreenHead(w, mode);
    await assert.rejects(w.ciDone(), /native merge head changed/);
    assert.equal(w.pr.state, 'open');
    assert.equal(w.nativeMerges.length, 0);
    assert.deepEqual(w.did('nativeMerge')[0][1], {id: 'PR_7', head: HEAD});
    assert.equal(w.did('nativeWait').length, 0);
  });
}

for (const labeled of [true, false]) {
  test(`legacy recovery disarms an inherited request before CI completes (label ${labeled})`, async () => {
    const w = setup(modes[0], {nativeAutoMethod: 'SQUASH'});
    Object.assign(w.runs[0], {status: 'in_progress', conclusion: null});
    if (!labeled) w.pr.labels = [];
    await w.sweep();
    assert.equal(w.nativeAutoMethod, null);
    assert.equal(w.did('nativeCancel').length, 1);
    assert.equal(w.nativeMerges.length, 0);
    assert.equal(w.did('nativeWait').length, 0);
  });
}

