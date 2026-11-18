'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const controller = require('./label-automerge.js');
const {world, authorWorld, HEAD, OTHER, T} = require('./automerge-fixture.js');

test('label captures approval and arms native waiting even before CI starts', async () => {
  const w = world({approved: false, runs: [], checks: []});
  await w.labeled();
  assert.equal(w.did('approve').length, 1);
  assert.equal(w.did('nativeWait').length, 1);
  assert.deepEqual(w.did('nativeWait')[0][1], {id: 'PR_7', head: HEAD});
  assert.equal(w.did('status').at(-1)[1].state, 'pending');
  for (const action of ['merge', 'dispatch']) assert.equal(w.did(action).length, 0);
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
  failed: w => {w.runs[0].conclusion = 'failure';},
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
  });
}

function forkWorld() {
  const w = world(); w.pr.head.repo.id = 20;
  for (const run of w.runs) run.head_repository.id = 20;
  return w;
}

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
