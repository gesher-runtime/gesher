'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const controller = require('./label-automerge.js');
const appStatus = require('./app-qa-status.js');
const {world, authorWorld, HEAD, OTHER, T, APP_BOT} = require('./automerge-fixture.js');

test('label captures approval and arms native waiting even before CI starts', async () => {
  const w = world({approved: false, runs: [], checks: []});
  await w.labeled();
  assert.equal(w.did('approve').length, 1);
  assert.equal(w.did('nativeWait').length, 1);
  assert.deepEqual(w.did('nativeWait')[0][1], {id: 'PR_7', head: HEAD});
  assert.equal(w.did('status').at(-1)[1].state, 'pending');
  for (const action of ['merge', 'dispatch']) assert.equal(w.did(action).length, 0);
});

test('the deployed repository requires the Gesher App before PR or status writes', async () => {
  const w = world({approved: false, repositoryId: 1426153186});
  await assert.rejects(w.labeled(), /Gesher GitHub App is required/);
  assert.deepEqual(w.calls, []);
});

test('the App writes the receipt and GitHub Actions writes the verified status', async () => {
  const w = world({approved: false, repositoryId: 1426153186});
  w.useApp();
  await w.labeled();
  assert.equal(w.did('app-approve').length, 1);
  assert.equal(w.did('approve').length, 0);
  assert.equal(w.did('nativeWait').length, 1);
  assert.deepEqual(w.reviews[0].user, APP_BOT);
  assert.equal(w.did('status').at(-1)[1].state, 'success');
});

test('the App dismisses stale receipts from either trusted bot on a new head', async () => {
  const w = world();
  w.useApp();
  w.seedApproval();
  w.reviews[1].user = {...APP_BOT};
  w.pr.head.sha = OTHER;
  await w.pushed();
  assert.equal(w.did('app-dismiss').length, 2);
  assert.equal(w.did('app-removeLabel').length, 1);
  assert.equal(w.did('dismiss').length, 0);
  assert.equal(w.did('removeLabel').length, 0);
  assert.equal(w.did('status').at(-1)[1].state, 'failure');
});

test('CI failure removes the label and posts the explanation as the App', async () => {
  const w = world();
  w.useApp();
  Object.assign(w.runs[0], {status: 'completed', conclusion: 'failure'});
  await w.ciDone();
  assert.equal(w.did('app-removeLabel').length, 1);
  assert.equal(w.did('app-comment').length, 1);
  assert.equal(w.did('removeLabel').length, 0);
  assert.equal(w.did('comment').length, 0);
  assert.deepEqual(w.comments.at(-1).user, APP_BOT);
  assert.equal(w.did('status').at(-1)[1].state, 'failure');
});

for (const [name, change] of Object.entries({
  'missing label': w => {w.pr.labels = [];},
  draft: w => {w.pr.draft = true;},
  'wrong base': w => {w.pr.base.ref = 'other';},
  'changed head': w => {w.pr.head.sha = OTHER;},
  'revoked access': w => {w.permission = 'read';},
  'wrong account identity': w => {w.permissionUserId = 999;},
  'dismissed receipt': w => {w.reviews[0].state = 'DISMISSED';},
  'edited receipt': w => {w.approvalNodeOverride = {lastEditedAt: T(40)};},
  'foreign bot receipt': w => {w.reviews[0].user = {...APP_BOT, id: 888};},
  'non-bot receipt': w => {w.reviews[0].user = {...APP_BOT, type: 'User'};},
  'missing receipt': w => {w.reviews = [];},
  'creation-time label': w => {w.events[0].created_at = T(0);},
  'reopened PR': w => {w.events.push({id: 60, event: 'reopened', created_at: T(40)});},
  'retargeted PR': w => {w.events.push({id: 60, event: 'base_ref_changed', created_at: T(40)});},
  'force-pushed PR': w => {w.events.push({id: 60, event: 'head_ref_force_pushed', created_at: T(40)});},
})) {
  test(name + ' cannot authorize the queue', async () => {
    const w = world(); change(w);
    assert.equal(await controller.approved(w.github, 'o', 'r', w.pr), false);
  });
}

test('author first label retains exact-head QA proof', async () => {
  const w = authorWorld({approved: false}); w.seedQa();
  await w.labeled();
  assert.equal(w.did('nativeWait').length, 1);
  assert.equal(w.did('reviewNode').length > 0, true);
});

for (const [name, change] of Object.entries({
  absent: w => {w.reviews = w.reviews.filter(r => r.state !== 'COMMENTED');},
  edited: w => {w.reviewNodeOverride = {lastEditedAt: T(25)};},
  'wrong head': w => {w.reviews.at(-1).commit_id = OTHER;},
  'same-second as label': w => {w.reviews.at(-1).submitted_at = T(30);},
  'wrong actor': w => {w.reviews.at(-1).user.id = 90;},
  'duplicate proof': w => {w.seedQa();},
  'extra marker field': w => {w.reviews.at(-1).body = w.reviews.at(-1).body.replace('"base":"main"', '"base":"main","extra":1');},
})) {
  test('author QA ' + name + ' fails closed', async () => {
    const w = authorWorld(); w.seedQa(); change(w);
    assert.equal(await controller.approved(w.github, 'o', 'r', w.pr), false);
  });
}

test('deliberate author relabel remains an explicit approval route', async () => {
  const w = authorWorld();
  w.events.unshift({...w.events[0], id: 40, created_at: T(10)});
  assert.equal(await controller.approved(w.github, 'o', 'r', w.pr), true);
});

test('removing a label revokes status and native waiting', async () => {
  const w = world({nativeAutoMethod: 'SQUASH'}); w.pr.labels = [];
  await w.fire('pull_request_target', {action: 'unlabeled', label: {name: 'automerge'}, pull_request: w.pr});
  assert.equal(w.did('status').at(-1)[1].state, 'failure');
  assert.equal(w.did('nativeCancel').length, 1);
  assert.equal(w.did('nativeWait').length, 0);
});

test('push cannot reuse receipt even if label remains', async () => {
  const w = world(); w.pr.head.sha = OTHER;
  await w.pushed();
  assert.equal(w.did('dismiss').length, 1);
  assert.equal(w.did('status').at(-1)[1].state, 'failure');
  assert.equal(w.did('nativeWait').length, 0);
  assert.equal(w.did('removeLabel').length, 1);
  assert.deepEqual(w.pr.labels, []);
});

test('a push removes an older label even before its approval job ran', async () => {
  const w = world({approved: false}); w.pr.head.sha = OTHER; w.pr.updated_at = T(40);
  await w.pushed();
  assert.equal(w.did('removeLabel').length, 1);
});

test('force-push invalidation removes an old label even when the head returns', async () => {
  const w = world();
  w.events.push({id: 60, event: 'head_ref_force_pushed', created_at: T(40)});
  await w.pushed();
  assert.equal(w.did('removeLabel').length, 1);
  assert.equal(w.did('nativeWait').length, 0);
});

test('a delayed push preserves a freshly applied label whose approval is pending', async () => {
  const w = world({approved: false}); w.pr.head.sha = OTHER;
  const push = structuredClone(w.pr); push.updated_at = T(40);
  w.events[0].created_at = T(41);
  await w.pushed(push);
  assert.equal(w.did('removeLabel').length, 0);
});

test('a delayed push preserves a fresh exact-head approval', async () => {
  const w = world(); w.pr.head.sha = OTHER; w.pr.updated_at = T(40);
  const push = structuredClone(w.pr);
  w.events.push({...w.events[0], id: 70, created_at: T(41)}); w.seedApproval();
  await w.pushed(push);
  assert.equal(w.did('removeLabel').length, 0);
});

for (const second of [39, 40, 41]) {
  test('an unrecorded label near the push timestamp stays unapproved: second ' + second, async () => {
    const w = world({approved: false}); w.pr.head.sha = OTHER;
    const push = structuredClone(w.pr); push.updated_at = T(40);
    w.events[0].created_at = T(second);
    await w.pushed(push);
    assert.equal(w.did('removeLabel').length, 0);
    assert.equal(w.did('nativeWait').length, 0);
    assert.equal(w.did('status').at(-1)[1].state, 'failure');
  });
}

test('an old push delivery cannot remove a label for a later head', async () => {
  const w = world({approved: false}); const push = structuredClone(w.pr);
  push.updated_at = T(50); w.pr.head.sha = OTHER;
  await w.pushed(push);
  assert.equal(w.did('removeLabel').length, 0);
});

test('a relabel during stale-label cleanup is preserved by event identity', async () => {
  const w = world(); w.pr.head.sha = OTHER;
  let reads = 0;
  w.hooks.events = () => {
    if (++reads === 2) w.events.push({...w.events[0], id: 70, created_at: T(41)});
  };
  await w.pushed();
  assert.equal(w.did('removeLabel').length, 0);
});

test('fork pushes remove stale labels using the target repository', async () => {
  const w = world(); w.pr.head.repo.id = 20; w.pr.head.sha = OTHER;
  await w.pushed();
  assert.equal(w.did('removeLabel').length, 1);
  assert.equal(w.did('removeLabel')[0][1].repo, 'r');
});

test('a head change during approval reads blocks authorization', async () => {
  const w = world(); w.hooks.approvalNode = () => {w.pr.head.sha = OTHER;};
  assert.equal(await controller.approved(w.github, 'o', 'r', structuredClone(w.pr)), false);
});

test('successful registered PR CI releases the gate without a second CI run', async () => {
  const w = world(); await w.ciDone();
  assert.equal(w.did('status').at(-1)[1].state, 'success');
  assert.equal(w.did('nativeWait').length, 1);
  assert.equal(w.did('dispatch').length, 0);
  assert.equal(w.did('merge').length, 0);
});

for (const [name, change] of Object.entries({
  missing: w => {w.runs = [];},
  pending: w => {w.runs[0].status = 'in_progress'; w.runs[0].conclusion = null;},
  manual: w => {w.runs[0].event = 'workflow_dispatch';},
  'different source repository': w => {w.runs[0].head_repository.id = 20;},
  'different target repository': w => {w.runs[0].repository.id = 20;},
  'wrong workflow identity': w => {w.runs[0].workflow_id = 90;},
  'wrong workflow path': w => {w.runs[0].path = '.github/workflows/other.yml';},
  'wrong branch': w => {w.runs[0].head_branch = 'other';},
  'missing gate': w => {w.checks = [];},
  'failed gate': w => {w.checks[0].conclusion = 'failure';},
  'different app': w => {w.checks[0].app.id = 1;},
  'different suite': w => {w.checks[0].check_suite.id = 90;},
  'invalid suite identity': w => {w.runs[0].check_suite_id = 0;},
})) {
  test(name + ' CI cannot release approval', async () => {
    const w = world(); change(w);
    await w.ciDone();
    assert.equal(w.did('status').at(-1)[1].state, 'pending');
    assert.equal(w.did('removeLabel').length, 0);
    assert.equal(w.did('comment').length, 0);
  });
}

function forkWorld() {
  const w = world(); w.pr.head.repo.id = 20;
  for (const run of w.runs) run.head_repository.id = 20;
  return w;
}

for (const index of [0, 1]) {
  test('fork workflow awaiting approval preserves label and resumes after CI: ' + index, async () => {
    const w = forkWorld();
    Object.assign(w.runs[index], {status: 'completed', conclusion: 'action_required'});
    w.checks = [];
    await w.ciDone();
    assert.equal(w.did('removeLabel').length, 0);
    assert.equal(w.did('comment').length, 0);
    assert.equal(w.did('status').at(-1)[1].state, 'pending');
    assert.equal(w.nativeAutoMethod, 'SQUASH');
    Object.assign(w.runs[index], {run_attempt: 2, conclusion: 'success'});
    w.checks = world().checks;
    await w.ciDone();
    assert.equal(w.did('status').at(-1)[1].state, 'success');
    assert.equal(w.did('removeLabel').length, 0);
    assert.equal(w.did('approve').length, 0);
  });
}

test('workflow-capable credential only enables and disables native waiting', async () => {
  const w = world({approved: false});
  const botGraphql = w.github.graphql;
  const mutations = [];
  w.github.graphql = async (query, args) => {
    assert.doesNotMatch(query, /mutation/);
    return botGraphql(query, args);
  };
  w.mergeGithub = {graphql: async (query, args) => {
    assert.match(query, /mutation/);
    assert.match(query, /enablePullRequestAutoMerge|disablePullRequestAutoMerge/);
    mutations.push(query);
    return botGraphql(query, args);
  }};
  await w.labeled();
  assert.equal(w.did('approve').length, 1);
  assert.equal(w.did('status').at(-1)[1].state, 'success');
  assert.deepEqual(w.did('nativeWait')[0][1], {id: 'PR_7', head: HEAD});
  w.pr.labels = [];
  await w.fire('pull_request_target', {action: 'unlabeled', label: {name: 'automerge'}, pull_request: w.pr});
  assert.equal(w.did('status').at(-1)[1].state, 'failure');
  assert.equal(w.nativeAutoMethod, null);
  assert.equal(mutations.length, 2);
});

test('a rejected workflow-capable credential cannot release the QA gate', async () => {
  const w = world();
  w.mergeGithub = {graphql: async () => { throw new Error('workflows permission required'); }};
  await assert.rejects(w.ciDone(), /workflows permission required/);
  assert.equal(w.did('status').at(-1)[1].state, 'pending');
  assert.equal(w.nativeAutoMethod, null);
});

test('maintainer-enabled squash waiting survives without another privileged mutation', async () => {
  const w = world({nativeAutoMethod: 'SQUASH'});
  w.mergeGithub = {graphql: async () => { assert.fail('existing squash waiting needs no mutation'); }};
  w.runs[0].status = 'in_progress'; w.runs[0].conclusion = null;
  await w.ciDone();
  assert.equal(w.did('status').at(-1)[1].state, 'pending');
  Object.assign(w.runs[0], {status: 'completed', conclusion: 'success'});
  await w.ciDone();
  assert.equal(w.did('status').at(-1)[1].state, 'success');
  assert.equal(w.nativeAutoMethod, 'SQUASH');
  assert.equal(w.did('nativeWait').length, 0);
  assert.equal(w.did('nativeCancel').length, 0);
});

test('a workflow-capable credential cannot authorize an unapproved head', async () => {
  const w = world(); w.pr.head.sha = OTHER;
  w.mergeGithub = {graphql: async () => { assert.fail('unapproved head must not enable waiting'); }};
  await w.ciDone(OTHER);
  assert.equal(w.did('status').at(-1)[1].state, 'failure');
  assert.equal(w.did('nativeWait').length, 0);
});

for (const conclusion of ['failure', 'timed_out', 'startup_failure']) {
  test(conclusion + ' revokes the label and native waiting with one short bot comment', async () => {
    const w = world({nativeAutoMethod: 'SQUASH'});
    w.runs[0].conclusion = conclusion;
    await w.ciDone();
    assert.deepEqual(w.pr.labels, []);
    assert.equal(w.did('removeLabel').length, 1);
    assert.equal(w.did('status').at(-1)[1].state, 'failure');
    assert.equal(w.nativeAutoMethod, null);
    assert.equal(w.did('nativeCancel').length, 1);
    assert.equal(w.did('comment').length, 1);
    assert.match(w.did('comment')[0][1].body, /^CI failed; removed `automerge`\./);
    assert.match(w.did('comment')[0][1].body, /https:\/\/github.com\/o\/r\/actions\/runs\/100/);
    assert.match(w.did('comment')[0][1].body, /gesher-automerge-ci-failed-v1/);
    await w.ciDone();
    assert.equal(w.did('comment').length, 1);
    assert.equal(w.did('dispatch').length, 0);
    assert.equal(w.did('merge').length, 0);
  });
}

test('ARM64 failure revokes approval even while the other workflow is pending', async () => {
  const w = world();
  w.runs[0].status = 'in_progress'; w.runs[0].conclusion = null;
  w.runs[1].conclusion = 'failure';
  await w.ciDone();
  assert.equal(w.did('removeLabel').length, 1);
  assert.match(w.did('comment')[0][1].body, /actions\/runs\/101/);
});

test('failure cleanup supports a fork without confusing source and target repositories', async () => {
  const w = forkWorld(); w.runs[0].conclusion = 'failure';
  await w.ciDone();
  assert.equal(w.did('removeLabel').length, 1);
  assert.equal(w.did('comment')[0][1].repo, 'r');
});

for (const state of ['in_progress', 'success']) {
  test('a delayed failure cannot revoke a current ' + state + ' rerun', async () => {
    const w = world(); w.runs[0].conclusion = 'failure';
    w.runDetailsOverride = {run_attempt: 2, status: state === 'success' ? 'completed' : state,
      conclusion: state === 'success' ? 'success' : null};
    await w.ciDone();
    assert.equal(w.did('removeLabel').length, 0);
    assert.equal(w.did('comment').length, 0);
    assert.equal(w.did('status').at(-1)[1].state, state === 'success' ? 'success' : 'pending');
  });
}

test('a rerun starting immediately before cleanup preserves the label', async () => {
  const w = world(); w.runs[0].conclusion = 'failure';
  w.hooks.runDetails = () => {
    if (w.did('runDetails').length === 2)
      Object.assign(w.runs[0], {run_attempt: 2, status: 'in_progress', conclusion: null});
  };
  await w.ciDone();
  assert.equal(w.did('removeLabel').length, 0);
  assert.equal(w.did('comment').length, 0);
});

test('a superseded failing run cannot revoke approval for a newer successful run', async () => {
  const w = world();
  const latest = {...structuredClone(w.runs[0]), id: 102, check_suite_id: 1002};
  w.runs[0].conclusion = 'failure'; w.runs.push(latest);
  w.checks[0].check_suite.id = 1002;
  await w.ciDone();
  assert.equal(w.did('removeLabel').length, 0);
  assert.equal(w.did('status').at(-1)[1].state, 'success');
});

test('a label reapplied during failed-run reads is preserved', async () => {
  const w = world(); w.runs[0].conclusion = 'failure';
  w.hooks.runDetails = () => {
    if (w.did('runDetails').length === 1)
      w.events.push({...w.events[0], id: 70, created_at: T(41)});
  };
  await w.ciDone();
  assert.equal(w.did('removeLabel').length, 0);
  assert.equal(w.did('comment').length, 0);
});

test('a push during failed-run cleanup preserves the newer head label', async () => {
  const w = world(); w.runs[0].conclusion = 'failure';
  w.hooks.runDetails = () => {
    if (w.did('runDetails').length === 2) w.pr.head.sha = OTHER;
  };
  await w.ciDone();
  assert.equal(w.did('removeLabel').length, 0);
  assert.equal(w.did('comment').length, 0);
});

test('a successful rerun after failure needs a fresh label', async () => {
  const w = world(); w.runs[0].conclusion = 'failure';
  await w.ciDone();
  Object.assign(w.runs[0], {run_attempt: 2, conclusion: 'success'});
  await w.ciDone();
  assert.equal(w.did('status').at(-1)[1].state, 'failure');
  assert.equal(w.nativeAutoMethod, null);
  assert.equal(w.did('comment').length, 1);
});

test('a collaborator label approves a fork head through the target repository', async () => {
  const w = forkWorld(); w.reviews = [];
  await w.labeled();
  assert.equal(w.did('approve').length, 1);
  assert.equal(w.did('nativeWait')[0][1].head, HEAD);
  assert.equal(w.did('status').at(-1)[1].state, 'success');
});

test('fork CI completion locates the PR without relying on live association arrays', async () => {
  const w = forkWorld(); await w.ciDone();
  assert.equal(w.did('status').at(-1)[1].state, 'success');
});

test('a workflow from another fork cannot wake approval for this PR', async () => {
  const w = forkWorld(); await w.ciDone(HEAD, 90);
  assert.equal(w.did('status').length, 0);
});

test('fork review events need no write token; CI completion rechecks QA', async () => {
  const w = forkWorld();
  await w.fire('pull_request_review', {action: 'edited', pull_request: w.pr});
  assert.equal(w.calls.length, 0);
  w.reviews[0].state = 'DISMISSED';
  await w.ciDone();
  assert.equal(w.did('status').at(-1)[1].state, 'failure');
});

test('proof revoked during CI reads cannot release the pending gate', async () => {
  const w = authorWorld(); w.seedQa();
  w.hooks.checks = () => {w.reviewNodeOverride = {lastEditedAt: T(40)};};
  await w.ciDone();
  assert.equal(w.did('status').at(-1)[1].state, 'failure');
  assert.equal(w.nativeAutoMethod, null);
});

test('a preexisting merge-method request is replaced with squash while the gate is pending', async () => {
  const w = world({nativeAutoMethod: 'MERGE'}); await w.ciDone();
  assert.equal(w.did('nativeCancel').length, 1);
  assert.equal(w.nativeAutoMethod, 'SQUASH');
  assert.equal(w.did('status')[0][1].state, 'pending');
});

test('targeted reconciliation reuses existing approval without creating reviews', async () => {
  const w = world();
  await w.fire('workflow_dispatch', {inputs: {pull_request_number: '7'}});
  assert.equal(w.did('listPRs').length, 0);
  assert.equal(w.did('approve').length, 0);
  assert.equal(w.did('status').at(-1)[1].state, 'success');
});

test('invalid targeted reconciliation is rejected before any API call', async () => {
  const w = world();
  await assert.rejects(w.fire('workflow_dispatch', {inputs: {pull_request_number: '7;other'}}));
  assert.equal(w.calls.length, 0);
});

function appStatusWorld(over = {}) {
  const w = world({repositoryId: 1426153186, appQaStatus: true,
    appId: appStatus.APP_ID, appInstallationId: appStatus.INSTALLATION_ID, ...over});
  w.useApp();
  return w;
}

function lastStatus(w, context) {
  return w.statuses.filter(s => s.context === context).sort((a, b) => b.id - a.id)[0];
}

function bothStates(w, state) {
  assert.equal(lastStatus(w, appStatus.STATUS)?.state, state);
  assert.equal(lastStatus(w, appStatus.APP_STATUS)?.state, state);
}

for (const flag of [undefined, false]) {
  test('App status is opt-in; disabled value ' + flag + ' retains the Actions gate only', async () => {
    const w = appStatusWorld({appQaStatus: flag, appId: undefined, appInstallationId: undefined});
    w.prGithub.rest.repos.createCommitStatus = () => assert.fail('disabled App status wrote');
    await w.ciDone();
    assert.equal(w.did('app-status').length, 0);
    assert.equal(w.did('statuses').length, 0);
    assert.equal(lastStatus(w, appStatus.STATUS).state, 'success');
    assert.equal(lastStatus(w, appStatus.STATUS).creator.id, 41898282);
  });
}

for (const [name, change] of Object.entries({
  'wrong App': w => {w.appId = 15368;},
  'wrong installation': w => {w.appInstallationId = 1;},
  'missing App': w => {w.prGithub = undefined;},
  'Actions client substituted for App': w => {w.prGithub = w.github;},
  'missing status capability': w => {delete w.prGithub.rest.repos.createCommitStatus;},
  'nonboolean flag': w => {w.appQaStatus = 'true';},
})) {
  test('App status configuration rejects ' + name + ' before writes', async () => {
    const w = appStatusWorld(); change(w);
    await assert.rejects(w.ciDone(), /Gesher GitHub App|verified App|explicit boolean/);
    assert.deepEqual(w.calls, []);
  });
}

test('verified App success precedes the unchanged Actions gate and is read back', async () => {
  const w = appStatusWorld({approved: false});
  await w.labeled();
  bothStates(w, 'success');
  assert.equal(w.did('app-approve').length, 1);
  assert.equal(w.did('approve').length, 0);
  assert.deepEqual(lastStatus(w, appStatus.APP_STATUS).creator, APP_BOT);
  assert.equal(lastStatus(w, appStatus.STATUS).creator.id, 41898282);
  const writes = w.calls.filter(([name]) => ['status', 'app-status'].includes(name));
  assert.deepEqual(writes.map(([name, a]) => [name, a.context, a.state]), [
    ['status', appStatus.STATUS, 'pending'], ['app-status', appStatus.APP_STATUS, 'pending'],
    ['app-status', appStatus.APP_STATUS, 'success'], ['status', appStatus.STATUS, 'success'],
  ]);
  assert.ok(writes.every(([, a]) => a.sha === HEAD && a.owner === 'o' && a.repo === 'r'));
  assert.ok(w.did('statuses').length >= 3);
  assert.equal(w.did('nativeWait')[0][1].head, HEAD);
  assert.equal(w.did('merge').length, 0);
});

for (const [name, change] of Object.entries({
  missing: w => {w.runs = [];},
  pending: w => {w.runs[0].status = 'in_progress'; w.runs[0].conclusion = null;},
  'approval required': w => {w.runs[0].conclusion = 'action_required';},
  cancelled: w => {w.runs[0].conclusion = 'cancelled';},
  'wrong CI source': w => {w.runs[0].head_repository.id = 99;},
  'wrong check source': w => {w.checks[0].app.id = appStatus.APP_ID;},
  'missing aggregate check': w => {w.checks = [];},
  'wrong aggregate suite': w => {w.checks[0].check_suite.id = 99;},
})) {
  test('App shadow remains pending for ' + name + ' CI evidence', async () => {
    const w = appStatusWorld(); change(w); await w.ciDone();
    bothStates(w, 'pending');
    assert.equal(w.did('app-status').filter(([, a]) => a.state === 'success').length, 0);
  });
}

for (const [name, change] of Object.entries({
  'missing label': w => {w.pr.labels = [];},
  draft: w => {w.pr.draft = true;},
  'wrong base': w => {w.pr.base.ref = 'other';},
  'missing receipt': w => {w.reviews = [];},
  'dismissed receipt': w => {w.reviews[0].state = 'DISMISSED';},
  'edited receipt': w => {w.approvalNodeOverride = {lastEditedAt: T(40)};},
  'wrong receipt actor': w => {w.reviews[0].user = {...APP_BOT, id: 1};},
  'revoked permission': w => {w.permission = 'read';},
  'force push': w => {w.events.push({id: 60, event: 'head_ref_force_pushed', created_at: T(40)});},
})) {
  test('App shadow fails closed for ' + name, async () => {
    const w = appStatusWorld({nativeAutoMethod: 'SQUASH'}); change(w); await w.ciDone();
    bothStates(w, 'failure');
    assert.equal(w.did('app-status').filter(([, a]) => a.state === 'success').length, 0);
    assert.equal(w.nativeAutoMethod, null);
  });
}

test('App status mirrors CI failure without changing guarded label removal or the failure receipt', async () => {
  const w = appStatusWorld({nativeAutoMethod: 'SQUASH'}); w.runs[0].conclusion = 'failure';
  await w.ciDone();
  bothStates(w, 'failure');
  assert.equal(w.did('app-removeLabel').length, 1);
  assert.equal(w.did('app-comment').length, 1);
  assert.match(w.comments.at(-1).body, /"label":50,"run":100,"attempt":1/);
  assert.equal(w.nativeAutoMethod, null);
});

for (const [name, change] of Object.entries({
  missing: w => {w.statuses = w.statuses.filter(s => s.context !== appStatus.APP_STATUS);},
  pending: w => {lastStatus(w, appStatus.APP_STATUS).state = 'pending';},
  failed: w => {lastStatus(w, appStatus.APP_STATUS).state = 'failure';},
  'foreign source': w => {lastStatus(w, appStatus.APP_STATUS).creator.id = 41898282;},
  'wrong account type': w => {lastStatus(w, appStatus.APP_STATUS).creator.type = 'User';},
  'wrong head': w => {lastStatus(w, appStatus.APP_STATUS).url = 'https://api.github.com/repos/o/r/statuses/' + OTHER;},
  'replaced status': w => {w.statuses.unshift({...lastStatus(w, appStatus.APP_STATUS), id: w.nextStatusId++});},
})) {
  test('App success readback ' + name + ' cannot release the existing gate', async () => {
    const w = appStatusWorld({nativeAutoMethod: 'SQUASH'});
    let changed = false;
    w.hooks.statuses = () => {
      if (!changed && lastStatus(w, appStatus.APP_STATUS)?.state === 'success') {changed = true; change(w);}
    };
    await assert.rejects(w.ciDone(), /App QA status/);
    assert.ok(changed);
    assert.equal(w.did('status').filter(([, a]) => a.state === 'success').length, 0);
    bothStates(w, 'failure');
    assert.equal(w.nativeAutoMethod, null);
  });
}

for (const [name, override] of Object.entries({
  'Actions writer': {creator: {login: 'github-actions[bot]', id: 41898282, type: 'Bot'}},
  'wrong context': {context: 'qa/something-else'},
  'wrong repository': {url: 'https://api.github.com/repos/other/r/statuses/' + HEAD},
  'invalid ID': {id: 0},
})) {
  test('App status response rejects ' + name + ' without an identity fallback', async () => {
    const w = appStatusWorld({nativeAutoMethod: 'SQUASH'});
    w.statusResponseOverride = {'app-status': override};
    await assert.rejects(w.ciDone(), /identity mismatch/);
    assert.equal(lastStatus(w, appStatus.STATUS).state, 'failure');
    assert.equal(w.did('status').filter(([, a]) => a.state === 'success').length, 0);
    assert.equal(w.nativeAutoMethod, null);
  });
}

test('missing App statuses permission revokes the legacy gate and cancels native waiting', async () => {
  const w = appStatusWorld({nativeAutoMethod: 'SQUASH'});
  w.statusErrors = {'app-status': new Error('statuses write permission denied')};
  await assert.rejects(w.ciDone(), /permission denied/);
  assert.equal(lastStatus(w, appStatus.STATUS).state, 'failure');
  assert.equal(w.did('app-status').at(-1)[1].state, 'failure');
  assert.equal(w.nativeAutoMethod, null);
});

test('a failing Actions credential still attempts to revoke the App status', async () => {
  const w = appStatusWorld({nativeAutoMethod: 'SQUASH'});
  w.statusErrors = {status: new Error('Actions statuses denied')};
  await assert.rejects(w.ciDone(), /Actions statuses denied/);
  assert.equal(lastStatus(w, appStatus.APP_STATUS).state, 'failure');
  assert.equal(w.did('status').at(-1)[1].state, 'failure');
  assert.equal(w.nativeAutoMethod, null);
});

for (const [name, revoke] of Object.entries({
  label: w => {w.pr.labels = [];},
  review: w => {w.reviews[0].state = 'DISMISSED';},
  permission: w => {w.permission = 'read';},
  'review edit': w => {w.approvalNodeOverride = {lastEditedAt: T(40)};},
})) {
  test('a ' + name + ' revocation during App success publication is rechecked', async () => {
    const w = appStatusWorld({nativeAutoMethod: 'SQUASH'});
    w.hooks['app-status'] = a => {if (a.state === 'success') revoke(w);};
    await w.ciDone();
    bothStates(w, 'failure');
    assert.equal(w.did('status').filter(([, a]) => a.state === 'success').length, 0);
    assert.equal(w.nativeAutoMethod, null);
  });
}

test('a push during App publication never writes success to the new head', async () => {
  const w = appStatusWorld();
  w.hooks['app-status'] = a => {if (a.state === 'success') w.pr.head.sha = OTHER;};
  await w.ciDone();
  bothStates(w, 'failure');
  assert.ok(w.calls.filter(([name]) => ['status', 'app-status'].includes(name)).every(([, a]) => a.sha === HEAD));
  assert.equal(w.did('status').filter(([, a]) => a.state === 'success').length, 0);
});

for (const conclusion of [null, 'failure']) {
  test('a new CI attempt during App publication restores ' + (conclusion ? 'failure' : 'pending'), async () => {
    const w = appStatusWorld();
    w.hooks['app-status'] = a => {
      if (a.state === 'success') Object.assign(w.runs[0], {run_attempt: 2,
        status: conclusion ? 'completed' : 'in_progress', conclusion});
    };
    await w.ciDone();
    bothStates(w, conclusion ? 'failure' : 'pending');
    assert.equal(w.did('status').filter(([, a]) => a.state === 'success').length, 0);
  });
}

test('a late CI failure cannot retain prepared App success when a rerun defeats label cleanup', async () => {
  const w = appStatusWorld();
  w.hooks['app-status'] = a => {if (a.state === 'success') w.runs[0].conclusion = 'failure';};
  w.hooks.runDetails = () => {
    if (w.did('runDetails').length === 4)
      Object.assign(w.runs[0], {run_attempt: 2, status: 'in_progress', conclusion: null});
  };
  await w.ciDone();
  bothStates(w, 'failure');
  assert.equal(w.did('app-removeLabel').length, 0);
  assert.equal(w.did('status').filter(([, a]) => a.state === 'success').length, 0);
});

test('a replaced App status during final approval reads is checked again before legacy success', async () => {
  const w = appStatusWorld({nativeAutoMethod: 'SQUASH'});
  let replaced = false;
  w.hooks.approvalNode = () => {
    if (!replaced && lastStatus(w, appStatus.APP_STATUS)?.state === 'success') {
      replaced = true;
      w.statuses.unshift({...lastStatus(w, appStatus.APP_STATUS), id: w.nextStatusId++, state: 'pending'});
    }
  };
  await assert.rejects(w.ciDone(), /App QA status is missing, replaced/);
  bothStates(w, 'failure');
  assert.equal(w.did('status').filter(([, a]) => a.state === 'success').length, 0);
  assert.equal(w.nativeAutoMethod, null);
});

test('App readback paginates unrelated status history rather than trusting a partial page', async () => {
  const w = appStatusWorld({statuses: Array.from({length: 120}, (_, i) => ({id: i + 1, context: 'unrelated/' + i}))});
  await w.ciDone();
  bothStates(w, 'success');
  assert.ok(w.did('statuses').some(([, a]) => a.page === 2));
});

test('combined status from another head cannot authorize App success', async () => {
  const w = appStatusWorld({nativeAutoMethod: 'SQUASH', statusReadbackHead: OTHER});
  await assert.rejects(w.ciDone(), /readback head mismatch/);
  bothStates(w, 'failure');
  assert.equal(w.did('status').some(([, a]) => a.state === 'success'), false);
  assert.equal(w.nativeAutoMethod, null);
});

test('manual App reconciliation requires an existing receipt and cannot manufacture one', async () => {
  const w = appStatusWorld({approved: false});
  await w.fire('workflow_dispatch', {inputs: {pull_request_number: '7'}});
  bothStates(w, 'failure');
  assert.equal(w.did('app-approve').length, 0);
  assert.equal(w.did('nativeWait').length, 0);
});

test('the App shadow retains exact-head author COMMENT QA before the first label', async () => {
  const w = appStatusWorld({approved: false});
  w.pr.user = {id: 42, login: 'maintainer'};
  w.seedQa();
  await w.labeled();
  bothStates(w, 'success');
  assert.equal(w.did('app-approve').length, 1);
});

test('missing author COMMENT QA cannot be replaced by a green App status', async () => {
  const w = appStatusWorld({approved: false});
  w.pr.user = {id: 42, login: 'maintainer'};
  await w.labeled();
  bothStates(w, 'failure');
  assert.equal(w.did('app-approve').length, 0);
  assert.equal(w.did('app-status').some(([, a]) => a.state === 'success'), false);
});

test('author COMMENT edited during App publication revokes both statuses', async () => {
  const w = appStatusWorld();
  w.pr.user = {id: 42, login: 'maintainer'};
  w.seedQa();
  w.hooks['app-status'] = a => {if (a.state === 'success') w.reviewNodeOverride = {lastEditedAt: T(40)};};
  await w.ciDone();
  bothStates(w, 'failure');
  assert.equal(w.did('status').some(([, a]) => a.state === 'success'), false);
});

test('source repository changed during App publication cannot release either gate', async () => {
  const w = appStatusWorld();
  w.hooks['app-status'] = a => {if (a.state === 'success') w.pr.head.repo.id = 20;};
  await w.ciDone();
  bothStates(w, 'failure');
  assert.equal(w.did('status').some(([, a]) => a.state === 'success'), false);
});

test('a CI metadata error after App success attempts both revocations and cancellation', async () => {
  const w = appStatusWorld({nativeAutoMethod: 'SQUASH'});
  w.hooks.runs = () => {
    if (lastStatus(w, appStatus.APP_STATUS)?.state === 'success') throw new Error('CI metadata unavailable');
  };
  await assert.rejects(w.ciDone(), /CI metadata unavailable/);
  bothStates(w, 'failure');
  assert.equal(w.nativeAutoMethod, null);
});

test('fork review events do not write the App shadow with a read-only event token', async () => {
  const w = appStatusWorld();
  w.pr.head.repo.id = 20;
  await w.fire('pull_request_review', {action: 'edited', pull_request: w.pr});
  assert.deepEqual(w.calls, []);
});

// Route reads to a separate client and make the Actions client reject them.
function splitReads(w) {
  const reads = [];
  const actionsOnly = new Set(['createCommitStatus', 'getCombinedStatusForRef', 'createReview',
    'dismissReview', 'removeLabel', 'createComment']);
  const rest = {};
  for (const [ns, fns] of Object.entries(w.github.rest)) {
    rest[ns] = {};
    for (const [name, fn] of Object.entries(fns)) {
      rest[ns][name] = async (args) => { reads.push(name); return fn(args); };
      if (!actionsOnly.has(name)) fns[name] = async () => assert.fail('GITHUB_TOKEN used for ' + name);
    }
  }
  const graphql = w.github.graphql;
  w.readGithub = {rest, graphql: async (...args) => { reads.push('graphql'); return graphql(...args); }};
  w.github.graphql = async (query, args) => {
    if (!query.includes('mutation')) assert.fail('GITHUB_TOKEN used for a GraphQL read');
    return graphql(query, args);
  };
  return reads;
}

for (const appQaStatus of [false, true]) {
  test(`evidence reads use the read client; the required status stays on Actions (App status ${appQaStatus})`, async () => {
    const w = appStatusWorld({appQaStatus});
    const reads = splitReads(w);
    await w.ciDone();
    assert.equal(lastStatus(w, appStatus.STATUS)?.state, 'success');
    assert.ok(w.did('status').every(([, a]) => a.context === appStatus.STATUS));
    for (const name of ['listEvents', 'getCollaboratorPermissionLevel', 'listReviews',
      'listWorkflowRunsForRepo', 'listForRef', 'getWorkflowRun', 'graphql']) assert.ok(reads.includes(name), name);
  });
}

test('workflow_run reconciles the PRs GitHub names and lists open PRs only for forks', async () => {
  const w = world({repositoryId: 10});
  await w.fire('workflow_run', {workflow_run: {event: 'pull_request', head_sha: HEAD,
    head_repository: {id: 10}, pull_requests: [{number: 7, head: structuredClone(w.pr.head),
      base: structuredClone(w.pr.base)}]}});
  assert.equal(w.did('listPRs').length, 0);
  assert.equal(w.did('status').at(-1)[1].state, 'success');

  const stale = world({repositoryId: 10});
  // A PR named for a different head than the run is not reconciled.
  await stale.fire('workflow_run', {workflow_run: {event: 'pull_request', head_sha: HEAD,
    head_repository: {id: 10}, pull_requests: [{number: 7, head: {...structuredClone(stale.pr.head), sha: OTHER},
      base: structuredClone(stale.pr.base)}]}});
  assert.deepEqual(stale.calls, []);

  const fork = world({repositoryId: 10});
  fork.pr.head.repo.id = 20;
  for (const r of fork.runs) r.head_repository.id = 20;
  await fork.ciDone(HEAD, 20);
  assert.equal(fork.did('listPRs').length, 1);
  assert.equal(fork.did('status').at(-1)[1].state, 'success');
});

test('native waiting is cancelled from REST pull request state without a GraphQL read', async () => {
  const w = world({nativeAutoMethod: 'SQUASH'}); w.pr.labels = [];
  const graphql = w.github.graphql;
  w.github.graphql = async (query, args) => {
    if (!query.includes('mutation')) assert.fail('unexpected GraphQL read');
    return graphql(query, args);
  };
  await w.pushed();
  assert.equal(w.nativeAutoMethod, null);
  assert.equal(w.did('nativeCancel').length, 1);
});

for (const file of ['ci.yml', 'arm64-windows.yml']) {
  for (const [name, override] of Object.entries({
    disabled: {state: 'disabled_manually'},
    replaced: {id: 999},
    relocated: {path: '.github/workflows/retired.yml'},
  })) {
    test(`${file} workflow registration ${name} during App publication restores both pending gates`, async () => {
      const w = appStatusWorld();
      splitReads(w);
      let changed = false;
      w.hooks['app-status'] = args => {if (args.state === 'success') changed = true;};
      const getWorkflow = w.readGithub.rest.actions.getWorkflow;
      w.readGithub.rest.actions.getWorkflow = async args => {
        const result = await getWorkflow(args);
        return changed && args.workflow_id === file ?
          {...result, data: {...result.data, ...override}} : result;
      };
      await w.ciDone();
      assert.ok(changed, 'the registration changed after the prepared App success');
      bothStates(w, 'pending');
      assert.equal(w.did('status').some(([, args]) => args.state === 'success'), false);
      assert.equal(w.did('app-removeLabel').length, 0);
    });
  }
}

test('workflow registration refresh failure after App publication revokes both gates without a reader fallback', async () => {
  const w = appStatusWorld({nativeAutoMethod: 'SQUASH'});
  splitReads(w);
  let changed = false;
  w.hooks['app-status'] = args => {if (args.state === 'success') changed = true;};
  const getWorkflow = w.readGithub.rest.actions.getWorkflow;
  w.readGithub.rest.actions.getWorkflow = async args => {
    if (changed) throw Object.assign(new Error('workflow read permission revoked'), {status: 403});
    return getWorkflow(args);
  };
  await assert.rejects(w.ciDone(), /workflow read permission revoked/);
  assert.ok(changed);
  bothStates(w, 'failure');
  assert.equal(w.did('status').some(([, args]) => args.state === 'success'), false);
  assert.equal(w.nativeAutoMethod, null);
});
