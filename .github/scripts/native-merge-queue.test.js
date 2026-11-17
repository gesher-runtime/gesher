'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const controller = require('./native-merge-queue.js');
const {world, authorWorld, HEAD, OTHER, T} = require('./merge-queue-fixture.js');

test('label captures approval and arms native waiting even before CI starts', async () => {
  const w = world({approved: false, runs: [], checks: []});
  await w.labeled();
  assert.equal(w.did('approve').length, 1);
  assert.equal(w.did('nativeWait').length, 1);
  assert.deepEqual(w.did('nativeWait')[0][1], {id: 'PR_7', head: HEAD});
  assert.equal(w.did('status').at(-1)[1].state, 'success');
  for (const action of ['merge', 'dispatch', 'runs', 'checks']) assert.equal(w.did(action).length, 0);
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

test('removing a label revokes status, native waiting and queue membership', async () => {
  const w = world(); w.pr.labels = [];
  await w.fire('pull_request_target', {action: 'unlabeled', label: {name: 'automerge'}, pull_request: w.pr});
  assert.equal(w.did('status').at(-1)[1].state, 'failure');
  assert.equal(w.did('nativeCancel').length, 2);
  assert.equal(w.did('nativeWait').length, 0);
});

test('push cannot reuse receipt even if label remains', async () => {
  const w = world(); w.pr.head.sha = OTHER;
  await w.pushed();
  assert.equal(w.did('dismiss').length, 1);
  assert.equal(w.did('status').at(-1)[1].state, 'failure');
  assert.equal(w.did('nativeWait').length, 0);
});

test('a missing native queue fails closed without a direct-merge fallback', async () => {
  const w = world(); const original = w.github.graphql;
  w.github.graphql = (q, a) => q.includes('mergeQueue(branch:') ? {repository: {mergeQueue: null}} : original(q, a);
  await assert.rejects(w.labeled(), /not enabled/);
  assert.equal(w.did('merge').length, 0);
  assert.equal(w.did('nativeWait').length, 0);
});

test('a head change during approval reads blocks authorization', async () => {
  const w = world(); w.hooks.approvalNode = () => {w.pr.head.sha = OTHER;};
  assert.equal(await controller.approved(w.github, 'o', 'r', structuredClone(w.pr)), false);
});

for (const mode of ['valid', 'missing', 'duplicate', 'truncated', 'unapproved']) {
  test('merge-group approval: ' + mode, async () => {
    const w = world(); const original = w.github.graphql;
    const entries = [{position: 1, headCommit: {oid: OTHER}, pullRequest: {number: 7}}];
    if (mode === 'missing') entries[0].headCommit.oid = HEAD;
    if (mode === 'duplicate') entries.push(entries[0]);
    if (mode === 'unapproved') w.pr.labels = [];
    w.github.graphql = (q, a) => q.includes('entries(first:') ?
      {repository: {mergeQueue: {entries: {nodes: entries, pageInfo: {hasNextPage: mode === 'truncated'}}}}} : original(q, a);
    const task = controller.groupApproval(w.github, 'o', 'r', {base_ref: 'refs/heads/main', head_sha: OTHER});
    if (mode === 'valid') await task;
    else await assert.rejects(task);
  });
}
