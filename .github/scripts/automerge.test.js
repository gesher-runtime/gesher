'use strict';
// Deterministic REST fixtures exercise the production controller without writes.
const test = require('node:test');
const assert = require('node:assert/strict');
const run = require('./automerge.js');
const HEAD = 'a'.repeat(40);
const OTHER = 'b'.repeat(40);
const BASE = 'c'.repeat(40);
const MERGE = 'd'.repeat(40);
const T = (second) => '2026-11-16T10:00:' + String(second).padStart(2, '0') + 'Z';
const clone = structuredClone;

function world(over = {}) {
  const w = {
    pr: {number: 7, state: 'open', draft: false, created_at: T(0), updated_at: T(30),
      user: {id: 99, login: 'contributor'},
      head: {sha: HEAD, ref: 'topic', repo: {id: 10}},
      base: {sha: BASE, ref: 'main', repo: {id: 10}}, labels: [{name: 'automerge'}]},
    events: [{id: 50, event: 'labeled', label: {name: 'automerge'}, created_at: T(30),
      actor: {id: 42, login: 'maintainer'}}],
    runs: [], checks: [{name: 'build', status: 'completed', conclusion: 'success'},
      {name: 'optional', status: 'completed', conclusion: 'skipped'}],
    statuses: [], comments: [], reviews: [], permission: 'write',
    mergeError: null, mergeResult: {merged: true}, calls: [], hooks: {}, warnings: [],
    mergeProof: {sha: MERGE, parents: [{sha: BASE}, {sha: HEAD}]},
    targetHead: BASE,
    comparison: {status: 'diverged', base_commit: {sha: BASE}, merge_base_commit: {sha: OTHER}},
    ...over,
  };
  if (!over.runs) w.runs = ['ci', 'arm64-windows'].map((name, i) => ({
    id: 100 + i, workflow_id: 1 + i, name, path: '.github/workflows/' + name + '.yml',
    event: 'pull_request', status: 'completed', conclusion: 'success', created_at: T(10),
    head_sha: HEAD, head_branch: 'topic', head_repository: {id: 10}, repository: {id: 10},
    pull_requests: [{number: 7, head: clone(w.pr.head), base: clone(w.pr.base)}],
    referenced_workflows: [{path: 'o/r/.github/workflows/source-identity.yml@' + MERGE,
      ref: 'refs/pull/7/merge', sha: MERGE}],
  }));
  w.seedApproval = (patch = {}) => {
    const event = w.events.filter((e) => e.event === 'labeled').at(-1);
    const value = {repository: w.pr.base.repo.id, number: w.pr.number, head: w.pr.head.sha,
      base: w.pr.base.ref, label: event.id, actor: event.actor.id, ...patch};
    w.reviews.push({id: 200 + w.reviews.length, node_id: 'BOT_' + w.reviews.length,
      user: {login: 'github-actions[bot]', type: 'Bot'}, state: 'APPROVED',
      commit_id: value.head, submitted_at: T(Math.max(31, Number(event.created_at.slice(17,19)) + 1)),
      body: '<!-- gesher-automerge-v1 ' + JSON.stringify(value) + ' -->'});
  };
  if (over.approved !== false) w.seedApproval();
  w.seedOpening = (boundary = T(0), patch = {}) => {
    const value = {repository: w.pr.base.repo.id, number: w.pr.number,
      created: w.pr.created_at, boundary, ...patch};
    w.comments.push({id: 200 + w.comments.length, user: {login: 'github-actions[bot]', type: 'Bot'},
      created_at: T(1), updated_at: T(1),
      body: '<!-- gesher-automerge-opening-v1 ' + JSON.stringify(value) + ' -->'});
  };
  if (over.initialized !== false) w.seedOpening();
  w.seedQa = (patch = {}, marker = {}) => {
    const value = {repository: w.pr.base.repo.id, number: w.pr.number,
      head: w.pr.head.sha, base: w.pr.base.ref, ...marker};
    const review = {id: 900 + w.reviews.length, node_id: 'PRR_' + (900 + w.reviews.length),
      user: {id: 42, login: 'maintainer'}, state: 'COMMENTED',
      commit_id: w.pr.head.sha, submitted_at: T(20),
      body: 'QA completed for this exact head.\n\n<!-- gesher-automerge-qa-v1 ' +
        JSON.stringify(value) + ' -->', ...patch};
    w.reviews.push(review);
    return review;
  };
  const rec = (name, fn) => async (args) => {
    w.calls.push([name, clone(args)]);
    if (w.hooks[name]) await w.hooks[name](args);
    return {data: clone(await fn(args))};
  };
  const page = (name, values, field) => rec(name, (a) => {
    const all = values(a);
    const items = all.slice((a.page - 1) * a.per_page, a.page * a.per_page);
    return field ? {[field]: items, total_count: all.length} : items;
  });
  const github = {rest: {
    pulls: {
      get: rec('getPR', () => w.pr),
      createReview: rec('approve', (args) => {
        if (w.approveError) throw w.approveError;
        const r = {id: 400 + w.reviews.length, node_id: 'BOT_' + w.reviews.length,
          user: {login: 'github-actions[bot]', type: 'Bot'}, state: 'APPROVED',
          submitted_at: T(Math.max(31, Number(w.events.at(-1).created_at.slice(17,19)) + 1)), commit_id: args.commit_id, body: args.body};
        w.reviews.push(r); return r;
      }),
      dismissReview: rec('dismiss', (args) => {
        const r = w.reviews.find((r) => r.id === args.review_id);
        r.state = 'DISMISSED'; return r;
      }),
      list: page('listPRs', () => [w.pr]),
      listReviews: page('reviews', () => {
        if (w.reviewError) throw w.reviewError;
        return w.reviews;
      }),
      merge: rec('merge', (args) => {
        if (w.mergeError) throw w.mergeError;
        if (args.sha !== w.pr.head.sha) throw Object.assign(new Error('head changed'), {status: 409});
        if (w.mergeResult.merged) w.pr.state = 'closed';
        return w.mergeResult;
      }),
    },
    issues: {
      listEvents: page('events', () => w.events),
      listComments: page('comments', () => w.comments),
      removeLabel: rec('removeLabel', () => {w.pr.labels = []; return {};}),
      createComment: rec('comment', (args) => {
        const item = {id: 300 + w.comments.length, user: {login: 'github-actions[bot]', type: 'Bot'},
          created_at: w.commentAt ?? T(31), updated_at: w.commentAt ?? T(31), body: args.body};
        w.comments.push(item);
        return item;
      }),
    },
    actions: {
      listWorkflowRunsForRepo: page('runs', (a) => w.runs.filter((r) => r.head_sha === a.head_sha), 'workflow_runs'),
      getWorkflow: rec('workflow', (a) => ({id: {'ci.yml': 1, 'arm64-windows.yml': 2, 'automerge.yml': 3}[a.workflow_id],
        state: 'active', path: '.github/workflows/' + a.workflow_id, ...w.workflowOverride})),
      getWorkflowRun: rec('runDetails', (a) => ({...w.runs.find((r) => r.id === a.run_id),
        ...w.runDetailsOverride})),
      createWorkflowDispatch: rec('dispatch', () => ({})),
    },
    checks: {listForRef: page('checks', () => w.checks, 'check_runs')},
    repos: {
      getCollaboratorPermissionLevel: rec('permission', () => {
        if (w.permissionError) throw w.permissionError;
        return {permission: w.permission, user: {id: w.permissionUserId ?? 42}};
      }),
      listCommitStatusesForRef: page('statuses', () => w.statuses),
      compareCommits: rec('compare', (args) => {
        if (w.comparisonError) throw w.comparisonError;
        return w.comparisons?.[args.base] ?? w.comparison;
      }),
    },
    git: {
      getCommit: rec('gitCommit', (args) => w.mergeProofs?.[args.commit_sha] ?? w.mergeProof),
      getRef: rec('gitRef', (args) => ({ref: 'refs/' + args.ref,
        object: {type: 'commit', sha: w.targetHead}, ...w.targetRefOverride})),
    },
  }};
  github.graphql = async (query, args) => {
    const bot = args.id.startsWith('BOT_');
    const name = bot ? 'approvalNode' : 'reviewNode';
    w.calls.push([name, clone(args)]);
    assert.match(query, /on PullRequestReview/);
    assert.match(query, /lastEditedAt/);
    if (w.hooks[name]) await w.hooks[name](args);
    if (!bot && w.reviewNodeError) throw w.reviewNodeError;
    const r = w.reviews.find((review) => review.node_id === args.id);
    return clone({node: r && (bot || !w.reviewNodeNull) ? {id: r.node_id, body: r.body,
      state: r.state, submittedAt: r.submitted_at, lastEditedAt: null,
      commit: {oid: r.commit_id}, ...(bot ? w.approvalNodeOverride : w.reviewNodeOverride)} : null});
  };
  const core = {info: () => {}, warning: (s) => w.warnings.push(s)};
  const fire = (eventName, payload) => run({github, core,
    context: {eventName, repo: {owner: 'o', repo: 'r'},
      payload: {repository: {default_branch: 'main'}, ...clone(payload)}}});
  w.labeled = (snapshot = w.pr) => fire('pull_request_target', {action: 'labeled',
    label: {name: 'automerge'}, sender: {id: 42}, pull_request: snapshot});
  w.pushed = (snapshot = w.pr) => fire('pull_request_target', {action: 'synchronize', pull_request: snapshot});
  w.ciDone = (sha = HEAD) => fire('workflow_run', {workflow_run: {head_sha: sha}});
  w.sweep = () => fire('schedule', {});
  w.fire = fire;
  w.did = (name) => w.calls.filter((c) => c[0] === name);
  return w;
}

function authorWorld(over = {}) {
  const w = world(over);
  w.pr.user = {id: 42, login: 'maintainer'};
  return w;
}

test('authorized label records head approval, merges exact head, then dispatches main CI', async () => {
  const w = world({approved: false});
  await w.labeled();
  assert.equal(w.did('comment').length, 0);
  assert.equal(w.did('approve').length, 1);
  assert.match(w.did('approve')[0][1].body, /"label":50,"actor":42/);
  assert.equal(w.did('merge').length, 1);
  assert.equal(w.did('merge')[0][1].sha, HEAD);
  assert.equal(w.did('merge')[0][1].merge_method, 'squash');
  assert.deepEqual(w.did('dispatch')[0][1], {owner: 'o', repo: 'r', workflow_id: 'ci.yml', ref: 'main'});
});

test('workflow completion uses the recorded approval and repeated delivery is harmless', async () => {
  const w = world();
  await w.ciDone();
  await w.ciDone();
  assert.equal(w.did('merge').length, 1);
});

for (const permission of ['read', 'none', 'triage', 'unknown']) {
  test('a label actor without merge access cannot delegate writes: ' + permission, async () => {
    const w = world({permission, approved: false});
    await w.labeled();
    assert.equal(w.did('merge').length, 0);
    assert.equal(w.did('removeLabel').length, 1);
    assert.equal(w.reviews.some((c) => c.body.includes('<!-- gesher-automerge-v1')), false);
  });
}
test('a missing collaborator or revoked permission blocks an existing approval', async () => {
  for (const patch of [{permission: 'read'}, {permissionError: Object.assign(new Error('not found'), {status: 404})}]) {
    const w = world(patch);
    await w.ciDone();
    assert.equal(w.did('merge').length, 0);
  }
});
test('an admin may opt in', async () => {
  const w = world({permission: 'admin', approved: false});
  await w.labeled();
  assert.equal(w.did('merge').length, 1);
});

test('a unique label delivery tolerates only zero or one second of forward update skew', async () => {
  for (const second of [29, 30, 31, 32]) {
    const w = world({approved: false}); w.pr.updated_at = T(second);
    await w.labeled();
    assert.equal(w.did('merge').length, second === 30 || second === 31 ? 1 : 0);
  }
});

test('same or adjacent-second label events cannot borrow a delivery timestamp', async () => {
  for (const second of [30, 31]) {
    const w = world({approved: false}); const old = clone(w.pr); old.updated_at = T(31);
    w.events.push({...clone(w.events[0]), id: 60, created_at: T(second)});
    w.pr.updated_at = T(31);
    await w.labeled(old);
    assert.equal(w.did('merge').length, 0);
    assert.equal(w.reviews.some((c) => c.body.includes('<!-- gesher-automerge-v1 ')), false);
  }
});

test('opening with an automerge label cannot approve a PR, even with merge access', async () => {
  for (const action of ['opened', 'labeled']) {
    for (const permission of ['read', 'write', 'admin']) {
      const w = world({permission, approved: false, initialized: false});
      w.pr.created_at = T(30);
      w.pr.user = {id: 42, login: 'maintainer'};
      await w.fire('pull_request_target', {action, label: {name: 'automerge'},
        sender: {id: 42}, pull_request: w.pr});
      assert.equal(w.did('removeLabel').length, 1);
      assert.equal(w.did('merge').length, 0);
      assert.equal(w.reviews.some((c) => c.body.includes('<!-- gesher-automerge-v1')), false);
    }
  }
});

test('CI and scheduled sweeps reject creation-time labels even with a receipt', async () => {
  for (const trigger of ['ciDone', 'sweep']) {
    const w = world({initialized: false}); w.pr.created_at = T(30);
    await w[trigger]();
    assert.equal(w.did('removeLabel').length, 1);
    assert.equal(w.did('merge').length, 0);
  }
});

test('missing or malformed opening metadata cannot authorize a label', async () => {
  for (const created of [undefined, null, '', 'not-a-date', T(40)]) {
    const w = world({approved: false, initialized: false}); w.pr.created_at = created;
    await w.labeled();
    assert.equal(w.did('removeLabel').length, 1);
    assert.equal(w.did('merge').length, 0);
  }
});

test('an opened delivery cannot capture approval or remove a later explicit label', async () => {
  const w = world({approved: false});
  const opened = {...clone(w.pr), updated_at: T(0), labels: []};
  await w.fire('pull_request_target', {action: 'opened', pull_request: opened});
  assert.equal(w.did('comment').length, 0);
  assert.equal(w.did('removeLabel').length, 0);
  assert.equal(w.did('merge').length, 0);
  await w.labeled();
  assert.equal(w.did('merge').length, 1);
});

test('a label observed in the opening snapshot is removed even after the creation second', async () => {
  for (const approved of [false, true]) {
    const w = world({approved});
    await w.fire('pull_request_target', {action: 'opened', pull_request: w.pr});
    assert.equal(w.did('removeLabel').length, 1);
    assert.equal(w.did('merge').length, 0);
    await w.labeled();
    assert.equal(w.did('merge').length, 0);
    assert.equal(w.did('approve').some((c) => c[1].body.includes('<!-- gesher-automerge-v1')), false);
  }
});

test('a delayed opening snapshot does not remove a replacement approval label', async () => {
  const w = world({approved: false}); const opened = clone(w.pr);
  w.events.push({...clone(w.events[0]), id: 60, created_at: T(40)});
  w.pr.updated_at = T(40);
  await w.fire('pull_request_target', {action: 'opened', pull_request: opened});
  assert.equal(w.did('removeLabel').length, 0);
  assert.equal(w.did('comment').length, 0);
  await w.labeled();
  assert.equal(w.did('merge').length, 1);
});

test('an author can apply their first label after an explicit exact-head QA review', async () => {
  for (const permission of ['write', 'admin']) {
    const w = world({permission, approved: false});
    w.pr.user = {id: 42, login: 'maintainer'};
    w.seedQa();
    await w.labeled();
    assert.equal(w.did('removeLabel').length, 0);
    assert.equal(w.did('merge').length, 1);
    assert.equal(w.reviews.filter((c) => c.body.includes('<!-- gesher-automerge-v1 ')).length, 1);
  }
});

test('an author first label after opening without QA cannot approve on any trigger', async () => {
  for (const trigger of ['labeled', 'ciDone', 'sweep']) {
    const w = authorWorld({approved: trigger !== 'labeled'});
    await w[trigger]();
    assert.equal(w.did('merge').length, 0);
    assert.equal(w.did('removeLabel').length, 1);
    assert.equal(w.did('approve').some((c) => c[1].body.includes('<!-- gesher-automerge-v1 ')), false);
  }
});

test('a deliberate author reapplication needs no QA proof query and uses stable actor identity', async () => {
  const w = authorWorld({approved: false});
  w.events.unshift({...clone(w.events[0]), id: 40, created_at: T(10),
    actor: {id: 42, login: 'previous-account-name'}});
  await w.labeled();
  assert.equal(w.did('merge').length, 1);
  assert.ok(w.did('reviews').length > 0);
  assert.equal(w.did('reviewNode').length, 0);
});

test('another account or an indistinguishable prior label cannot supply author reapplication', async () => {
  for (const patch of [
    {actor: {id: 99, login: 'maintainer'}}, {actor: {id: '42', login: 'maintainer'}},
    {created_at: T(30)}, {created_at: '2026-11-16T09:59:59Z'},
  ]) {
    const w = authorWorld({approved: false});
    w.events.unshift({...clone(w.events[0]), id: 40, created_at: T(10), ...patch});
    await w.labeled();
    assert.equal(w.did('merge').length, 0);
  }
});

test('ordinary collaborator first labels do not require author QA', async () => {
  const w = world({approved: false});
  await w.labeled();
  assert.equal(w.did('merge').length, 1);
  assert.ok(w.did('reviews').length > 0);
});

test('QA proof requires submitted COMMENTED review identity and strict ordering', async () => {
  for (const patch of [
    {id: undefined}, {id: 0}, {id: -1}, {id: '900'}, {id: Number.MAX_SAFE_INTEGER + 1},
    {node_id: undefined}, {node_id: ''}, {node_id: 900},
    {user: {id: 99, login: 'maintainer'}}, {user: {id: '42'}}, {user: {id: 0}},
    {commit_id: OTHER}, {state: 'APPROVED'}, {state: 'CHANGES_REQUESTED'},
    {state: 'PENDING'}, {state: 'DISMISSED'}, {state: undefined},
    {submitted_at: T(0)}, {submitted_at: T(1)}, {submitted_at: T(30)}, {submitted_at: T(31)},
    {submitted_at: 'invalid'}, {submitted_at: undefined}, {submitted_at: null},
    {body: undefined}, {body: null}, {body: 42}, {body: 'QA passed; please merge this head'},
  ]) {
    const w = authorWorld({approved: false}); w.seedQa(patch);
    await w.labeled();
    assert.equal(w.did('merge').length, 0, JSON.stringify(patch));
    assert.equal(w.did('approve').some((c) => c[1].body.includes('<!-- gesher-automerge-v1 ')), false);
  }
});

test('QA marker binds exactly the repository, PR, head and target with no extra fields', async () => {
  for (const marker of [
    {repository: 11}, {repository: '10'}, {number: 8}, {number: '7'},
    {head: OTHER}, {base: 'release'}, {extra: true},
    {repository: undefined}, {number: undefined}, {head: undefined}, {base: undefined},
  ]) {
    const w = authorWorld({approved: false}); w.seedQa({}, marker);
    await w.labeled();
    assert.equal(w.did('merge').length, 0, JSON.stringify(marker));
  }
});

test('missing, malformed, duplicate or non-standalone QA markers cannot authorize', async () => {
  const marker = '<!-- gesher-automerge-qa-v1 ' +
    JSON.stringify({repository: 10, number: 7, head: HEAD, base: 'main'}) + ' -->';
  for (const body of [
    '', 'QA passed', '<!-- gesher-automerge-qa-v1 broken -->',
    '<!-- gesher-automerge-qa-v1 null -->', '<!-- gesher-automerge-qa-v1 [] -->',
    marker + '\n' + marker, marker + ' ' + marker, 'prefix ' + marker, marker + ' suffix',
    ' ' + marker, marker.replace('"repository":10', '"repository":11,"repository":10'),
    marker.replace('"repository":10', '"repository": 10'), marker.slice(0, -4),
  ]) {
    const w = authorWorld({approved: false}); w.seedQa({body});
    await w.labeled();
    assert.equal(w.did('merge').length, 0, body);
  }
});

test('QA allows a standalone CRLF marker, compact reordered fields and renamed account login', async () => {
  const w = authorWorld({approved: false});
  w.seedQa({user: {id: 42, login: 'old-login'},
    body: 'Concrete QA result.\r\n\r\n<!-- gesher-automerge-qa-v1 ' +
      JSON.stringify({base: 'main', head: HEAD, number: 7, repository: 10}) + ' -->\r\n'});
  await w.labeled();
  assert.equal(w.did('merge').length, 1);
});

test('multiple or conflicting qualifying QA reviews are ambiguous', async () => {
  for (const marker of [{}, {base: 'release'}]) {
    const w = authorWorld({approved: false}); w.seedQa(); w.seedQa({}, marker);
    await w.labeled();
    assert.equal(w.did('merge').length, 0);
  }
});

test('QA review pagination includes later proof but never silently truncates excessive history', async () => {
  const w = authorWorld({approved: false});
  w.reviews = Array.from({length: 100}, (_, i) => ({id: i + 1, body: 'Discussion only'}));
  w.seedQa(); await w.labeled();
  assert.equal(w.did('merge').length, 1);
  assert.ok(w.did('reviews').some(([, args]) => args.page === 2));
  const huge = authorWorld();
  huge.reviews = Array.from({length: 1001}, (_, i) => ({id: i + 1}));
  await huge.ciDone();
  assert.equal(huge.did('merge').length, 0);
  assert.equal(huge.did('reviews').length, 10);
  assert.equal(huge.warnings.length, 1);
});

test('edited, absent or inconsistent GraphQL review metadata blocks REST proof', async () => {
  for (const reviewNodeOverride of [
    {id: 'another-node'}, {body: 'changed body'}, {state: 'DISMISSED'},
    {commit: {oid: OTHER}}, {commit: null}, {submittedAt: T(19)}, {submittedAt: undefined},
    {lastEditedAt: T(21)}, {lastEditedAt: T(35)}, {lastEditedAt: undefined},
  ]) {
    const w = authorWorld({approved: false, reviewNodeOverride}); w.seedQa();
    await w.labeled();
    assert.equal(w.did('merge').length, 0, JSON.stringify(reviewNodeOverride));
    assert.equal(w.did('approve').some((c) => c[1].body.includes('<!-- gesher-automerge-v1 ')), false);
  }
  const missing = authorWorld({approved: false, reviewNodeNull: true}); missing.seedQa();
  await missing.labeled(); assert.equal(missing.did('merge').length, 0);
});

test('REST or GraphQL review errors cannot fall back to first-author approval', async () => {
  for (const field of ['reviewError', 'reviewNodeError']) {
    for (const status of [403, 404, 422, 503]) {
      for (const trigger of ['labeled', 'ciDone']) {
        const w = authorWorld({approved: trigger !== 'labeled',
          [field]: Object.assign(new Error('review unavailable'), {status})});
        w.seedQa();
        if (trigger === 'labeled') await assert.rejects(w.labeled(), /review unavailable/);
        else await w.ciDone();
        assert.equal(w.did('merge').length, 0);
        assert.equal(w.did('approve').some((c) => c[1].body.includes('<!-- gesher-automerge-v1 ')), false);
      }
    }
  }
});

test('a QA marker cannot replace the bot approval review, including on legacy PRs', async () => {
  for (const patch of [{approved: false}, {approved: false, initialized: false}]) {
    const w = authorWorld(patch); w.seedQa();
    await w.ciDone();
    assert.equal(w.did('merge').length, 0);
    assert.equal(w.did('approve').some((c) => c[1].body.includes('<!-- gesher-automerge-v1 ')), false);
  }
});

test('author QA does not weaken permission, draft or unsuccessful CI gates', async () => {
  for (const mutate of [(w) => {w.permission = 'read';},
    (w) => {w.pr.draft = true;}, (w) => {w.runs[0].conclusion = 'failure';},
    (w) => {w.runs[0].status = 'in_progress';}]) {
    const w = authorWorld(); w.seedQa(); mutate(w);
    await w.ciDone(); assert.equal(w.did('merge').length, 0);
  }
});

test('removed, replaced, edited or revoked QA proof during CI queries blocks merging', async () => {
  for (const mutate of [
    (w) => {w.reviews = [];}, (w) => {w.reviews.at(-1).state = 'DISMISSED';},
    (w) => {w.reviews.at(-1).body = 'Proof withdrawn';},
    (w) => {w.reviews.at(-1).body += '\nA later edit';},
    (w) => {w.reviews.at(-1).id++; w.reviews.at(-1).node_id = 'replacement';},
    (w) => {w.reviews.at(-1).commit_id = OTHER;}, (w) => {w.reviews.at(-1).user.id = 99;},
    (w) => {w.reviews.at(-1).submitted_at = T(30);},
    (w) => {w.reviewNodeOverride = {lastEditedAt: T(35)};},
  ]) {
    const w = authorWorld(); w.seedQa(); w.hooks.statuses = () => mutate(w);
    await w.ciDone(); assert.equal(w.did('merge').length, 0);
  }
});

test('proof is revalidated after label capture and before final authorization and mutable state', async () => {
  const edited = authorWorld({approved: false}); edited.seedQa();
  edited.hooks.approve = (args) => {
    if (args.body.includes('<!-- gesher-automerge-v1 ')) edited.reviewNodeOverride = {lastEditedAt: T(31)};
  };
  await edited.labeled(); assert.equal(edited.did('merge').length, 0);
  for (const mutate of [
    (w) => {w.pr.head.sha = OTHER;}, (w) => {w.pr.base.ref = 'release';},
    (w) => {w.pr.base.sha = OTHER;}, (w) => {w.targetHead = OTHER;},
    (w) => {w.pr.draft = true;}, (w) => {w.pr.state = 'closed';},
    (w) => {w.pr.labels = [];}, (w) => {w.permission = 'read';},
    (w) => {w.reviews = w.reviews.filter((c) => !c.body.includes('<!-- gesher-automerge-v1 '));},
    (w) => {w.seedOpening(T(40));},
    (w) => {w.events.push({...clone(w.events[0]), id: 60, created_at: T(40)});},
  ]) {
    const w = authorWorld(); w.seedQa(); let queries = 0;
    w.hooks.reviewNode = () => {if (++queries === 2) mutate(w);};
    await w.ciDone();
    assert.equal(queries, 2);
    assert.equal(w.did('merge').length, 0);
  }
  const valid = authorWorld(); valid.seedQa(); await valid.ciDone();
  assert.equal(valid.did('merge').length, 1);
  assert.equal(valid.did('reviewNode').length, 2);
  const lastProof = valid.calls.findLastIndex(([name]) => name === 'reviewNode');
  for (const name of ['permission', 'comments', 'gitRef', 'getPR', 'events']) {
    assert.ok(lastProof < valid.calls.findLastIndex(([call]) => call === name), name);
  }
});

test('a valid QA proof and author first-label receipt work on every trigger', async () => {
  for (const trigger of ['labeled', 'ciDone', 'sweep']) {
    const w = world(); w.pr.user = {id: 42, login: 'maintainer'};
    w.seedQa();
    await w[trigger]();
    assert.equal(w.did('removeLabel').length, 0);
    assert.equal(w.did('merge').length, 1);
  }
});

test('an earlier author label cannot authorize an application before initialization', async () => {
  const w = world({approved: false}); w.pr.user = {id: 42, login: 'maintainer'};
  w.events.unshift({...clone(w.events[0]), id: 40, created_at: T(10)});
  w.seedOpening(T(35));
  await w.labeled();
  assert.equal(w.did('merge').length, 0);
  assert.equal(w.did('removeLabel').length, 1);
});

test('a first author label is found through complete paginated history without a prior label', async () => {
  const w = world({approved: false}); w.pr.user = {id: 42, login: 'maintainer'};
  w.seedQa();
  const current = clone(w.events[0]);
  w.events = [...Array.from({length: 100}, (_, i) => ({id: 2 + i, event: 'assigned', created_at: T(10)})),
  {...current, id: 150}];
  await w.labeled();
  assert.equal(w.did('merge').length, 1);
  assert.ok(w.did('events').some((call) => call[1].page === 2));
});

test('missing or malformed author identity cannot authorize any label', async () => {
  for (const user of [undefined, null, {}, {id: '99'}, {id: 0}, {id: -1},
    {id: Number.MAX_SAFE_INTEGER + 1}]) {
    for (const trigger of ['labeled', 'ciDone', 'sweep']) {
      const w = world(); w.pr.user = user;
      await w[trigger]();
      assert.equal(w.did('removeLabel').length, 1);
      assert.equal(w.did('merge').length, 0);
    }
  }
});

test('valid author identity remains required at the final merge read', async () => {
  for (const mutate of [
    (w) => {delete w.pr.user;},
    (w) => {w.pr.user.id = 0;},
    (w) => {w.pr.user.id = '42';},
  ]) {
    const w = world(); w.pr.user = {id: 42, login: 'maintainer'};
    w.seedQa();
    let reads = 0;
    w.hooks.getPR = () => {if (++reads === 2) mutate(w);};
    await w.ciDone();
    assert.equal(w.did('merge').length, 0);
    assert.equal(reads, 2);
  }
});

test('a first author label must be strictly later than the immutable receipt creation time', async () => {
  for (const recordedSecond of [29, 40, 41]) {
    const w = world({approved: false}); w.pr.user = {id: 42, login: 'maintainer'};
    w.pr.updated_at = T(40); w.events[0].created_at = T(40);
    w.seedQa({submitted_at: T(35)});
    w.comments[0].created_at = T(recordedSecond); w.comments[0].updated_at = T(recordedSecond);
    await w.labeled();
    assert.equal(w.did('merge').length, recordedSecond < 40 ? 1 : 0);
    assert.equal(w.did('removeLabel').length, recordedSecond < 40 ? 0 : 1);
  }
});

test('invalid or pre-PR receipt creation timestamps cannot grant approval', async () => {
  for (const at of ['invalid', '2026-11-16T09:59:59Z']) {
    const w = world();
    const opening = w.comments.find((c) => c.body.includes('gesher-automerge-opening-v1'));
    opening.created_at = at; opening.updated_at = at;
    await w.ciDone();
    assert.equal(w.did('merge').length, 0);
    assert.equal(w.warnings.length, 1);
  }
});

test('a later concurrent initialization receipt cannot relax the final approval boundary', async () => {
  const w = world();
  w.hooks.checks = () => {
    w.seedOpening(T(0));
    w.comments.at(-1).created_at = T(40); w.comments.at(-1).updated_at = T(40);
  };
  await w.ciDone();
  assert.equal(w.did('merge').length, 0);
});

test('the maximum of concurrent opening receipts governs regardless of list order', async () => {
  for (const reverse of [false, true]) {
    const w = world(); w.seedOpening(T(35));
    if (reverse) w.comments.reverse();
    await w.ciDone();
    assert.equal(w.did('merge').length, 0);
    assert.equal(w.did('removeLabel').length, 1);
  }
});

test('a stricter opening receipt recorded during CI checks blocks the final merge', async () => {
  const w = world(); w.hooks.runs = () => w.seedOpening(T(40));
  await w.ciDone();
  assert.equal(w.did('merge').length, 0);
});

test('human and other-bot opening comments cannot raise the legacy boundary', async () => {
  for (const user of [{login: 'maintainer', type: 'User'}, {login: 'other[bot]', type: 'Bot'}]) {
    const w = world(); w.comments[0].user = user;
    w.comments[0].body = w.comments[0].body.replace(T(0), T(40));
    await w.ciDone(); assert.equal(w.did('merge').length, 1);
  }
});

test('editing a stricter opening receipt cannot lower the effective boundary', async () => {
  const w = world(); w.seedOpening(T(40)); w.comments.at(-1).updated_at = T(50);
  await w.ciDone();
  assert.equal(w.did('merge').length, 0);
  assert.equal(w.warnings.length, 1);
});

test('stale PR base metadata does not replace the current Git target reference', async () => {
  const w = world(); w.pr.base.sha = OTHER;
  await w.ciDone();
  assert.equal(w.did('merge').length, 1);
  assert.deepEqual(w.did('gitRef')[0][1], {owner: 'o', repo: 'r', ref: 'heads/main'});
});

test('an unproven old target tip cannot pass through stale PR base metadata', async () => {
  const w = world(); w.targetHead = OTHER;
  await w.ciDone();
  assert.equal(w.did('merge').length, 0);
});

test('a reviewed passing head remains mergeable when its tested base is an ancestor of the target', async () => {
  const w = world({targetHead: OTHER,
    comparison: {status: 'ahead', base_commit: {sha: BASE}, merge_base_commit: {sha: BASE}}});
  await w.ciDone();
  assert.equal(w.did('merge').length, 1);
  assert.equal(w.did('merge')[0][1].sha, HEAD);
  assert.equal(w.did('compare').length, 2);
  for (const [, args] of w.did('compare')) {
    assert.deepEqual(args, {owner: 'o', repo: 'r', base: BASE, head: OTHER, per_page: 1});
  }
});

test('CI against the current target needs no ancestry query', async () => {
  const w = world(); await w.ciDone();
  assert.equal(w.did('merge').length, 1);
  assert.equal(w.did('compare').length, 0);
});

test('rewound, divergent, unrelated or malformed target history cannot reuse old CI', async () => {
  const valid = {status: 'ahead', base_commit: {sha: BASE}, merge_base_commit: {sha: BASE}};
  for (const comparison of [
    {...valid, status: 'behind'}, {...valid, status: 'diverged'},
    {...valid, status: 'identical'}, {...valid, status: 'unknown'},
    {...valid, base_commit: {sha: OTHER}}, {...valid, merge_base_commit: {sha: OTHER}},
    {...valid, base_commit: null}, {...valid, merge_base_commit: null}, {}, null,
  ]) {
    const w = world({targetHead: OTHER, comparison}); await w.ciDone();
    assert.equal(w.did('merge').length, 0);
    assert.equal(w.did('removeLabel').length, 0);
  }
});

test('unavailable ancestry evidence never grants a merge', async () => {
  for (const status of [403, 404, 422, 503]) {
    const w = world({targetHead: OTHER,
      comparisonError: Object.assign(new Error('comparison unavailable'), {status})});
    await w.ciDone();
    assert.equal(w.did('merge').length, 0);
    assert.equal(w.warnings.length, 1);
  }
});

test('each required workflow must independently prove its tested target history', async () => {
  const secondMerge = 'e'.repeat(40);
  const foreignBase = 'f'.repeat(40);
  for (const foreignIndex of [0, 1]) {
    const w = world({targetHead: OTHER,
      comparison: {status: 'ahead', base_commit: {sha: BASE}, merge_base_commit: {sha: BASE}}});
    w.runs[foreignIndex].referenced_workflows = [{
      path: 'o/r/.github/workflows/source-identity.yml@' + secondMerge,
      ref: 'refs/pull/7/merge', sha: secondMerge,
    }];
    w.mergeProofs = {[secondMerge]: {sha: secondMerge, parents: [{sha: foreignBase}, {sha: HEAD}]}};
    w.comparisons = {[foreignBase]: {status: 'diverged',
      base_commit: {sha: foreignBase}, merge_base_commit: {sha: BASE}}};
    await w.ciDone();
    assert.equal(w.did('merge').length, 0);
  }
});

test('fork CI with empty live associations can prove an ancestor base through immutable metadata', async () => {
  const w = world({targetHead: OTHER,
    comparison: {status: 'ahead', base_commit: {sha: BASE}, merge_base_commit: {sha: BASE}}});
  w.pr.head.repo.id = 20;
  w.runs.forEach((r) => {r.head_repository.id = 20; r.pull_requests = [];});
  await w.ciDone();
  assert.equal(w.did('merge').length, 1);
});

test('target advancement during ancestry verification defers the merge', async () => {
  const w = world({targetHead: OTHER,
    comparison: {status: 'ahead', base_commit: {sha: BASE}, merge_base_commit: {sha: BASE}}});
  w.hooks.compare = () => {w.targetHead = 'e'.repeat(40);};
  await w.ciDone();
  assert.equal(w.did('merge').length, 0);
});

test('the target Git reference is reread after CI checks', async () => {
  const w = world(); w.hooks.runs = () => {w.targetHead = OTHER;};
  await w.ciDone();
  assert.equal(w.did('merge').length, 0);
});

test('missing or misidentified target Git references fail closed', async () => {
  for (const targetRefOverride of [{ref: 'refs/heads/other'}, {object: {type: 'tag', sha: BASE}},
    {object: {type: 'commit', sha: 'invalid'}}]) {
    const w = world({targetRefOverride}); await w.ciDone();
    assert.equal(w.did('merge').length, 0);
    assert.equal(w.warnings.length, 1);
  }
});

test('mutations during the final opening receipt read remain visible to final state checks', async () => {
  for (const mutate of [
    (w) => {w.pr.base.ref = 'other';},
    (w) => {w.pr.labels = [];},
    (w) => {w.pr.head.sha = OTHER;},
  ]) {
    const w = world(); let reads = 0;
    w.hooks.comments = () => {if (++reads === 3) mutate(w);};
    await w.ciDone();
    assert.equal(reads, 3);
    assert.equal(w.did('merge').length, 0);
  }
});

test('malformed or conflicting opening identities block approval', async () => {
  for (const patch of [{repository: 99}, {number: 8}, {created: T(1)},
    {boundary: 'invalid'}, {extra: true}]) {
    const w = world(); w.seedOpening(T(0), patch);
    await w.ciDone();
    assert.equal(w.did('merge').length, 0);
    assert.equal(w.warnings.length, 1);
  }
});

test('a delayed opening event preserves a later label when the PR changed since opening', async () => {
  const w = world({approved: false}); const opened = clone(w.pr);
  opened.updated_at = T(0); opened.head.sha = OTHER;
  await w.fire('pull_request_target', {action: 'opened', pull_request: opened});
  assert.equal(w.did('removeLabel').length, 0);
  assert.equal(w.did('comment').length, 0);
  assert.equal(w.did('merge').length, 0);
});

test('a label with no recorded head cannot be inferred by a sweep or CI completion', async () => {
  const w = world({approved: false});
  await w.ciDone(); await w.sweep();
  assert.equal(w.did('merge').length, 0);
  assert.equal(w.did('comment').length, 0);
});
test('human, other-bot and dismissed reviews cannot supply approval', async () => {
  for (const edit of [
    (c) => {c.user = {login: 'maintainer', type: 'User'};},
    (c) => {c.user.login = 'other[bot]';},
    (c) => {c.state = 'DISMISSED';},
  ]) {
    const w = world(); edit(w.reviews[0]);
    await w.ciDone();
    assert.equal(w.did('merge').length, 0);
  }
});
test('conflicting or malformed receipts fail closed; identical duplicate receipts are safe', async () => {
  const duplicate = world(); duplicate.seedApproval(); await duplicate.ciDone();
  assert.equal(duplicate.did('merge').length, 1);
  const conflict = world(); conflict.seedApproval({head: OTHER}); await conflict.ciDone();
  assert.equal(conflict.did('merge').length, 0);
  const malformed = world(); malformed.reviews[0].body = '<!-- gesher-automerge-v1 broken -->';
  await malformed.ciDone();
  assert.equal(malformed.did('merge').length, 0);
  assert.equal(malformed.warnings.length, 1);
});
test('a push invalidates the old receipt even when its synchronize delivery is missing', async () => {
  const w = world(); w.pr.head.sha = OTHER;
  await w.ciDone(OTHER);
  assert.equal(w.did('merge').length, 0);
  assert.equal(w.did('removeLabel').length, 1);
});
test('a push removes the old approval; an unlabelled push does not write', async () => {
  const w = world(); w.pr.head.sha = OTHER; w.pr.updated_at = T(40);
  await w.pushed();
  assert.equal(w.did('removeLabel').length, 1);
  assert.equal(w.did('merge').length, 0);
  const absent = world(); absent.pr.labels = []; await absent.pushed();
  assert.equal(absent.did('removeLabel').length, 0);
  assert.equal(absent.did('comment').length, 0);
});
test('a delayed synchronize does not remove a fresh head approval', async () => {
  const w = world(); const old = clone(w.pr);
  w.pr.head.sha = OTHER; w.pr.updated_at = T(40);
  w.events.push({...clone(w.events[0]), id: 60, created_at: T(40)});
  w.seedApproval();
  await w.pushed(old);
  assert.equal(w.did('removeLabel').length, 0);
});
test('a delayed synchronize preserves a later relabel whose capture is still queued', async () => {
  const w = world({approved: false}); const pushed = clone(w.pr);
  pushed.updated_at = T(35);
  w.pr.updated_at = T(40);
  w.events.push({...clone(w.events[0]), id: 60, created_at: T(40)});
  await w.pushed(pushed);
  assert.equal(w.did('removeLabel').length, 0);
});
test('an old label delivery cannot approve a new head, or a later label after a head returns', async () => {
  const moved = world({approved: false}); const old = clone(moved.pr);
  moved.pr.head.sha = OTHER; await moved.labeled(old);
  assert.equal(moved.did('comment').length, 0);
  const returned = world({approved: false});
  const oldLabel = clone(returned.pr);
  returned.events.push({...clone(returned.events[0]), id: 60, created_at: T(40)});
  returned.pr.updated_at = T(40);
  await returned.labeled(oldLabel);
  assert.equal(returned.did('comment').length, 0);
});
test('ambiguous same-second relabels and mismatched senders cannot record approval', async () => {
  const w = world({approved: false});
  w.events.push({...clone(w.events[0]), id: 60});
  await w.labeled();
  assert.equal(w.did('comment').length, 0);
  const sender = world({approved: false});
  sender.events[0].actor.id = 77; // Neither the delivered sender nor the author.
  await sender.labeled();
  assert.equal(sender.did('comment').length, 0);
});
test('reopen or label removal invalidates the prior approval', async () => {
  for (const event of ['reopened', 'unlabeled']) {
    const w = world();
    w.events.push({id: 60, event, label: {name: 'automerge'}, created_at: T(40)});
    await w.ciDone();
    assert.equal(w.did('merge').length, 0);
  }
});

for (const [status, conclusion] of [['queued', null], ['in_progress', null], ['waiting', null],
  ['completed', 'failure'], ['completed', 'cancelled'], ['completed', 'skipped'],
  ['completed', 'neutral'], ['completed', 'action_required'], ['completed', 'startup_failure']]) {
  test('required CI must really complete successfully: ' + status + '/' + conclusion, async () => {
    const w = world(); Object.assign(w.runs[0], {status, conclusion});
    await w.ciDone();
    assert.equal(w.did('merge').length, 0);
  });
}
test('missing CI, missing ARM64 CI, and zero checks all block', async () => {
  for (const edit of [(w) => {w.runs = [];},
    (w) => {w.runs = w.runs.filter((r) => r.workflow_id !== 1);},
    (w) => {w.runs = w.runs.filter((r) => r.workflow_id !== 2);},
    (w) => {w.checks = [];}]) {
    const w = world(); edit(w); await w.ciDone();
    assert.equal(w.did('merge').length, 0);
  }
});
test('a display-name collision cannot substitute for the registered required workflow', async () => {
  const w = world(); w.runs[0].workflow_id = 999;
  await w.ciDone(); assert.equal(w.did('merge').length, 0);
});
test('trusted workflow identity requires path, event, repository, head repository and branch', async () => {
  for (const patch of [{path: '.github/workflows/unrelated.yml'}, {event: 'workflow_dispatch'},
    {repository: {id: 99}}, {head_repository: {id: 99}}, {head_branch: 'other'},
    {head_sha: OTHER}]) {
    const w = world(); Object.assign(w.runs[0], patch); await w.ciDone();
    assert.equal(w.did('merge').length, 0, JSON.stringify(patch));
  }
});
test('inactive or renamed registered workflows do not qualify', async () => {
  for (const workflowOverride of [{state: 'disabled_manually'}, {path: '.github/workflows/other.yml'}]) {
    const w = world({workflowOverride}); await w.ciDone();
    assert.equal(w.did('merge').length, 0);
  }
});
test('another PR or retargeted association cannot substitute for this PR', async () => {
  for (const edit of [(p) => {p.number = 99;}, (p) => {p.base.ref = 'release';},
    (p) => {p.head.sha = OTHER;}, (p) => {p.base.repo.id = 99;}]) {
    const w = world(); edit(w.runs[0].pull_requests[0]); await w.ciDone();
    assert.equal(w.did('merge').length, 0);
  }
});
test('fork runs with empty associations still require the tested head/base pair', async () => {
  for (const base of [BASE, OTHER]) {
    const w = world();
    w.pr.head.repo.id = 20;
    w.runs.forEach((r) => {r.head_repository.id = 20; r.pull_requests = [];});
    w.mergeProof.parents[0].sha = base;
    await w.ciDone();
    assert.equal(w.did('merge').length, base === BASE ? 1 : 0);
  }
});
test('live run associations cannot falsely prove a historical tested base', async () => {
  const w = world();
  w.mergeProof.parents[0].sha = OTHER;
  w.runs.forEach((r) => {r.pull_requests[0].base.sha = BASE;});
  await w.ciDone(); assert.equal(w.did('merge').length, 0);
});
test('a successful retry does not invalidate approval of the unchanged head', async () => {
  const w = world(); w.runs[0].conclusion = 'failure';
  await w.ciDone(); assert.equal(w.did('merge').length, 0);
  w.runs.push({...clone(w.runs[0]), id: 500, created_at: T(40), conclusion: 'success'});
  await w.ciDone(); assert.equal(w.did('merge').length, 1);
});
test('a same-ID rerun uses its current pending or completed result', async () => {
  const w = world(); Object.assign(w.runs[0], {run_attempt: 2, status: 'in_progress', conclusion: null});
  await w.ciDone(); assert.equal(w.did('merge').length, 0);
  Object.assign(w.runs[0], {status: 'completed', conclusion: 'success'});
  await w.ciDone(); assert.equal(w.did('merge').length, 1);
});
test('a pending or failing additional workflow blocks, even with a colliding display name', async () => {
  for (const patch of [{status: 'in_progress', conclusion: null}, {conclusion: 'failure'}]) {
    const w = world();
    w.runs.push({...clone(w.runs[0]), id: 200, workflow_id: 3, ...patch});
    await w.ciDone(); assert.equal(w.did('merge').length, 0);
  }
});
test('a scheduled sweep observes a manual workflow or external check finishing last', async () => {
  for (const external of ['workflow', 'check', 'status']) {
    const w = world();
    if (external === 'workflow') w.runs.push({...clone(w.runs[0]), id: 200,
      workflow_id: 3, event: 'workflow_dispatch', status: 'in_progress', conclusion: null});
    if (external === 'check') w.checks.push({name: 'external', status: 'in_progress', conclusion: null});
    if (external === 'status') w.statuses.push({id: 100, context: 'external', state: 'pending', created_at: T(20)});
    await w.ciDone(); assert.equal(w.did('merge').length, 0);
    if (external === 'workflow') Object.assign(w.runs.at(-1), {status: 'completed', conclusion: 'success'});
    if (external === 'check') Object.assign(w.checks.at(-1), {status: 'completed', conclusion: 'success'});
    if (external === 'status') w.statuses.push({id: 101, context: 'external', state: 'success', created_at: T(40)});
    await w.sweep(); assert.equal(w.did('merge').length, 1);
  }
});
test('pending, failed, cancelled and timed-out check runs block', async () => {
  for (const [status, conclusion] of [['in_progress', null], ['completed', 'failure'],
    ['completed', 'cancelled'], ['completed', 'timed_out']]) {
    const w = world(); w.checks.push({name: 'external', status, conclusion});
    await w.ciDone(); assert.equal(w.did('merge').length, 0);
  }
});
test('all current status contexts must succeed; newer values replace older ones', async () => {
  for (const state of ['pending', 'failure', 'error']) {
    const w = world({statuses: [{id: 1, context: 'ext', state, created_at: T(20)}]});
    await w.ciDone(); assert.equal(w.did('merge').length, 0);
    w.statuses.push({id: 2, context: 'ext', state: 'success', created_at: T(40)});
    await w.ciDone(); assert.equal(w.did('merge').length, 1);
  }
});
test('check pagination includes a failure on the second page', async () => {
  const w = world(); w.checks = Array.from({length: 100}, (_, i) =>
    ({name: 'job' + i, status: 'completed', conclusion: 'success'}));
  w.checks.push({name: 'last', status: 'completed', conclusion: 'failure'});
  await w.ciDone();
  assert.equal(w.did('checks').length, 2);
  assert.equal(w.did('merge').length, 0);
});
test('oversized or malformed histories fail closed with bounded requests', async () => {
  const w = world();
  w.events = Array.from({length: 1001}, (_, i) => ({id: i + 1, event: 'assigned', created_at: T(20)}));
  await w.ciDone();
  assert.equal(w.did('events').length, 10);
  assert.equal(w.did('merge').length, 0);
  assert.equal(w.warnings.length, 1);
  const missing = world(); delete missing.runs[0].created_at;
  await missing.ciDone();
  assert.equal(missing.did('merge').length, 0);
});
test('head, base, target, state, draft and label are rechecked after CI queries', async () => {
  for (const mutate of [(w) => {w.pr.head.sha = OTHER;},
    (w) => {w.pr.base.sha = OTHER;}, (w) => {w.pr.base.ref = 'release';},
    (w) => {w.pr.state = 'closed';}, (w) => {w.pr.draft = true;},
    (w) => {w.pr.labels = [];}]) {
    const w = world(); w.hooks.statuses = () => mutate(w);
    await w.ciDone(); assert.equal(w.did('merge').length, 0);
  }
});
test('relabel, permission revocation and receipt deletion during CI queries block', async () => {
  for (const mutate of [(w) => {w.events.push({...clone(w.events[0]), id: 60, created_at: T(40)});},
    (w) => {w.permission = 'read';}, (w) => {w.reviews = [];}]) {
    const w = world(); w.hooks.statuses = () => mutate(w);
    await w.ciDone(); assert.equal(w.did('merge').length, 0);
  }
});
test('a stale removal request cannot remove a newer label event', async () => {
  const w = world({permission: 'read', approved: false});
  let calls = 0;
  w.hooks.events = () => {
    if (++calls === 2) w.events.push({...clone(w.events[0]), id: 60, created_at: T(40)});
  };
  await w.labeled();
  assert.equal(w.did('removeLabel').length, 0);
});
test('a push racing the merge is rejected by the SHA pin and does not dispatch', async () => {
  const w = world(); w.hooks.merge = () => {w.pr.head.sha = OTHER;};
  await w.ciDone();
  assert.equal(w.did('merge')[0][1].sha, HEAD);
  assert.equal(w.did('dispatch').length, 0);
  assert.equal(w.did('removeLabel').length, 1);
});
test('merge rejection or a false success result never dispatches main CI', async () => {
  for (const patch of [{mergeError: Object.assign(new Error('conflict'), {status: 405})},
    {mergeError: Object.assign(new Error('head changed'), {status: 409})},
    {mergeResult: {merged: false}}]) {
    const w = world(patch); await w.ciDone();
    assert.equal(w.did('dispatch').length, 0);
    assert.equal(w.did('removeLabel').length, 1);
  }
});
test('a competing merge cannot turn an already merged PR into a failure comment', async () => {
  const w = world({mergeError: Object.assign(new Error('already merged'), {status: 405})});
  w.hooks.merge = () => {w.pr.state = 'closed';};
  await w.ciDone();
  assert.equal(w.did('removeLabel').length, 0);
  assert.equal(w.did('comment').length, 0);
  assert.equal(w.did('dispatch').length, 0);
});
test('draft, closed, unlabelled and stale-head events never merge', async () => {
  for (const patch of [{draft: true}, {state: 'closed'}, {labels: []}]) {
    const w = world(); Object.assign(w.pr, patch);
    await w.ciDone(); assert.equal(w.did('merge').length, 0);
  }
  const stale = world(); await stale.ciDone(OTHER);
  assert.equal(stale.did('merge').length, 0);
});
test('a different label is ignored before any API call', async () => {
  const w = world();
  await w.fire('pull_request_target', {action: 'labeled', label: {name: 'bug'}, pull_request: w.pr});
  assert.equal(w.calls.length, 0);
});
test('a non-default target preserves its approved target and does not dispatch main', async () => {
  const w = world({approved: false});
  w.pr.base.ref = 'release';
  w.runs.forEach((r) => {r.pull_requests[0].base.ref = 'release';});
  await w.labeled();
  assert.equal(w.did('merge').length, 1);
  assert.equal(w.did('dispatch').length, 0);
});

test('permission lookup must identify the same account as the label event', async () => {
  const w = world({permissionUserId: 99, approved: false});
  await w.labeled();
  assert.equal(w.did('merge').length, 0);
  assert.equal(w.did('removeLabel').length, 1);
});
test('force pushes and target changes invalidate an approval even when the head returns', async () => {
  for (const event of ['head_ref_force_pushed', 'base_ref_changed', 'automatic_base_change_succeeded']) {
    const w = world();
    w.events.push({id: 60, event, created_at: T(40)});
    await w.ciDone();
    assert.equal(w.did('merge').length, 0);
    assert.equal(w.did('removeLabel').length, 1);
  }
});
test('ambiguous label history cannot use a previously captured receipt', async () => {
  const w = world({approved: false});
  w.events.push({...clone(w.events[0]), id: 60});
  w.seedApproval();
  await w.ciDone();
  assert.equal(w.did('merge').length, 0);
});
test('mutations during final receipt reading are visible to the last state check', async () => {
  for (const mutate of [(w) => {w.pr.labels = [];}, (w) => {w.pr.draft = true;},
    (w) => {w.pr.base.sha = OTHER;}, (w) => {w.permission = 'read';},
    (w) => {w.events.push({...clone(w.events[0]), id: 60, created_at: T(40)});}]) {
    const w = world(); let count = 0;
    w.hooks.comments = () => {if (++count === 2) mutate(w);};
    await w.ciDone();
    assert.equal(w.did('merge').length, 0);
  }
});
test('a rerun starting between listing and fetching its details blocks', async () => {
  const w = world({runDetailsOverride: {status: 'in_progress', conclusion: null}});
  await w.ciDone();
  assert.equal(w.did('merge').length, 0);
});
test('every required workflow needs immutable local merge-source metadata', async () => {
  for (const index of [0, 1]) {
    for (const edit of [
      (r) => {delete r.referenced_workflows;},
      (r) => {r.referenced_workflows = [];},
      (r) => {r.referenced_workflows[0].ref = 'refs/pull/99/merge';},
      (r) => {r.referenced_workflows[0].ref = 'refs/heads/main';},
      (r) => {r.referenced_workflows[0].path = 'other/repo/.github/workflows/id.yml@' + MERGE;},
      (r) => {r.referenced_workflows[0].path = 'o/r/.github/workflows/id.yml@' + OTHER;},
      (r) => {r.referenced_workflows.push({path: 'o/r/.github/workflows/other.yml@' + OTHER,
        ref: 'refs/pull/7/merge', sha: OTHER});},
    ]) {
      const w = world(); edit(w.runs[index]); await w.ciDone();
      assert.equal(w.did('merge').length, 0);
    }
  }
});
test('the immutable merge object must have a valid proven base and the exact approved head', async () => {
  for (const mergeProof of [{sha: OTHER, parents: [{sha: BASE}, {sha: HEAD}]},
    {sha: MERGE, parents: [{sha: OTHER}, {sha: HEAD}]},
    {sha: MERGE, parents: [{sha: BASE}, {sha: OTHER}]},
    {sha: MERGE, parents: [{sha: 'invalid'}, {sha: HEAD}]},
    {sha: MERGE, parents: [{sha: BASE}]},
    {sha: MERGE, parents: [{sha: BASE}, {sha: HEAD}, {sha: OTHER}]},
    {sha: MERGE, parents: [{sha: HEAD}, {sha: BASE}]}]) {
    const w = world({mergeProof}); await w.ciDone();
    assert.equal(w.did('merge').length, 0);
  }
});
test('only the registered trusted controller and its check suites are excluded', async () => {
  const own = {...clone(world().runs[0]), id: 300, workflow_id: 3, name: 'automerge',
    path: '.github/workflows/automerge.yml', event: 'workflow_run',
    status: 'in_progress', conclusion: null, check_suite_id: 500};
  const w = world();
  w.runs.push(own);
  w.checks.push({name: 'automerge', status: 'in_progress', conclusion: null, check_suite: {id: 500}});
  await w.ciDone(); assert.equal(w.did('merge').length, 1);
  for (const patch of [{workflow_id: 999}, {event: 'pull_request'},
    {path: '.github/workflows/unrelated.yml'}]) {
    const blocked = world(); blocked.runs.push({...clone(own), ...patch});
    await blocked.ciDone(); assert.equal(blocked.did('merge').length, 0);
  }
  const named = world(); named.runs.push(own);
  named.checks.push({name: 'automerge', status: 'completed', conclusion: 'failure', check_suite: {id: 999}});
  await named.ciDone(); assert.equal(named.did('merge').length, 0);
});
test('the controller check alone cannot satisfy the no-checks guard', async () => {
  const w = world();
  w.runs.push({...clone(w.runs[0]), id: 300, workflow_id: 3,
    path: '.github/workflows/automerge.yml', event: 'schedule', check_suite_id: 500,
    status: 'in_progress', conclusion: null});
  w.checks = [{name: 'automerge', status: 'in_progress', conclusion: null, check_suite: {id: 500}}];
  await w.ciDone(); assert.equal(w.did('merge').length, 0);
});

for (const status of ['queued', 'in_progress']) {
  test('an older workflow rerun still blocks when a newer run is green: ' + status, async () => {
    const w = world();
    const newer = {...clone(w.runs[0]), id: 500, created_at: T(40)};
    Object.assign(w.runs[0], {run_attempt: 2, status, conclusion: null});
    w.runs.push(newer);
    // Latest check-name results can still be green; active runs are a separate gate.
    assert.equal(w.checks.every((c) => c.status === 'completed'), true);
    await w.ciDone();
    assert.equal(w.did('merge').length, 0);
    Object.assign(w.runs[0], {status: 'completed', conclusion: 'failure'});
    await w.ciDone();
    assert.equal(w.did('merge').length, 1, 'completed older failures remain superseded');
  });
}


test('new openings and unlabelled sweeps stay silent without an initialization record', async () => {
  const w = world({approved: false, initialized: false});
  w.pr.labels = []; w.pr.updated_at = T(0);
  await w.fire('pull_request_target', {action: 'opened', pull_request: w.pr});
  await w.sweep(); await w.sweep();
  assert.equal(w.did('comment').length, 0);
  assert.equal(w.did('approve').length, 0);
  assert.equal(w.comments.length, 0);
  w.pr.labels = [{name: 'automerge'}]; w.pr.updated_at = T(30);
  await w.labeled();
  assert.equal(w.did('approve').length, 1);
  assert.equal(w.did('merge').length, 1);
});

test('pending CI captures one native approval, without posting comments on retries', async () => {
  const w = world({approved: false}); w.runs[0].status = 'in_progress';
  await w.labeled(); await w.labeled(); await w.ciDone(); await w.sweep();
  assert.equal(w.did('comment').length, 0);
  assert.equal(w.did('approve').length, 1);
  assert.equal(w.did('merge').length, 0);
  w.runs[0].status = 'completed'; await w.ciDone();
  assert.equal(w.did('merge')[0][1].merge_method, 'squash');
});

test('pushing dismisses only stale controller approvals, even without a label', async () => {
  for (const labeled of [true, false]) {
    const w = world();
    w.reviews.push({...clone(w.reviews[0]), id: 901, node_id: 'HUMAN', user: {id: 42, type: 'User'}});
    w.pr.head.sha = OTHER; w.pr.updated_at = T(40);
    if (!labeled) w.pr.labels = [];
    await w.pushed();
    assert.equal(w.did('dismiss').length, 1);
    assert.equal(w.reviews[0].state, 'DISMISSED');
    assert.equal(w.reviews[1].state, 'APPROVED');
    assert.equal(w.did('comment').length, 0);
    assert.equal(w.did('merge').length, 0);
  }
});

test('dismissed or edited bot approval cannot be restored by replaying the label delivery', async () => {
  for (const edit of [(w) => {w.reviews[0].state = 'DISMISSED';},
    (w) => {w.approvalNodeOverride = {lastEditedAt: T(35)};}]) {
    const w = world(); edit(w);
    await w.labeled(); await w.ciDone();
    assert.equal(w.did('approve').length, 0);
    assert.equal(w.did('merge').length, 0);
    assert.equal(w.did('comment').length, 0);
  }
});

test('approval API rejection fails closed without a comment fallback', async () => {
  const w = world({approved: false, approveError: Object.assign(new Error('approval disabled'), {status: 403})});
  await assert.rejects(w.labeled(), /approval disabled/);
  assert.equal(w.did('merge').length, 0);
  assert.equal(w.did('comment').length, 0);
});

test('legacy approval comments cannot authorize a merge', async () => {
  const w = world();
  w.comments.push({...clone(w.comments[0]), body: w.reviews[0].body});
  w.reviews = [];
  await w.ciDone(); await w.sweep();
  assert.equal(w.did('merge').length, 0);
});


test('a fresh PR author can record opt-in with QA without any initialization comment', async () => {
  const w = authorWorld({approved: false, initialized: false}); w.seedQa();
  await w.labeled();
  assert.equal(w.did('approve').length, 1);
  assert.equal(w.did('merge').length, 1);
  assert.equal(w.comments.length, 0);
});

test('a delayed dismissal preserves an approval if its head becomes current again', async () => {
  const w = world(); w.pr.head.sha = OTHER;
  let reads = 0;
  w.hooks.getPR = () => {if (++reads === 2) w.pr.head.sha = HEAD;};
  await w.pushed();
  assert.equal(w.did('dismiss').length, 0);
  assert.equal(w.reviews[0].state, 'APPROVED');
});
