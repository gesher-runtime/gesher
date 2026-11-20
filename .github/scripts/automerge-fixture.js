'use strict';
// Deterministic REST fixtures exercise the production controller without writes.
const test = require('node:test');
const assert = require('node:assert/strict');
const run = require('./label-automerge.js');
const HEAD = 'a'.repeat(40);
const OTHER = 'b'.repeat(40);
const BASE = 'c'.repeat(40);
const MERGE = 'd'.repeat(40);
const ACTIONS_BOT = {login: 'github-actions[bot]', id: 41898282, type: 'Bot'};
const APP_BOT = {login: 'gesher-runtime-automation[bot]', id: 358279050, type: 'Bot'};
const T = (second) => '2026-11-16T10:00:' + String(second).padStart(2, '0') + 'Z';
const clone = structuredClone;

function world(over = {}) {
  const w = {
    pr: {node_id: 'PR_7', number: 7, state: 'open', draft: false, created_at: T(0), updated_at: T(30),
      user: {id: 99, login: 'contributor'},
      head: {sha: HEAD, ref: 'topic', repo: {id: 10}},
      base: {sha: BASE, ref: 'main', repo: {id: 10}}, labels: [{name: 'automerge'}]},
    events: [{id: 50, event: 'labeled', label: {name: 'automerge'}, created_at: T(30),
      actor: {id: 42, login: 'maintainer'}}],
    runs: [], checks: ['ci-required', 'arm64-required'].map((name, i) => ({id: 10 + i,
      name, status: 'completed', conclusion: 'success', app: {id: 15368}, check_suite: {id: 1000 + i}})),
    statuses: [], nextStatusId: 10000, comments: [], reviews: [], dispatches: [], permission: 'write', nativeAutoMethod: null,
    mergeError: null, mergeResult: {merged: true}, calls: [], hooks: {}, warnings: [],
    mergeProof: {sha: MERGE, parents: [{sha: BASE}, {sha: HEAD}]},
    targetHead: BASE,
    comparison: {status: 'diverged', base_commit: {sha: BASE}, merge_base_commit: {sha: OTHER}},
    ...over,
  };
  // Opt-in native policy model: status persistence and enabling auto-merge can
  // close a PR before the controller's next awaited read or cleanup executes.
  // The default recording-only fixture remains useful for ordinary unit cases.
  w.nativeMerges = [];
  const nativeBoundary = async (stage, args) => {
    if (!w.nativePolicy) return;
    if (w.nativePolicyHook) await w.nativePolicyHook(stage, args);
    if (w.pr.state !== 'open' || w.pr.draft || w.nativeAutoMethod !== 'SQUASH') return;
    const contexts = w.nativePolicy.contexts;
    const green = contexts.every(context => {
      const writer = context === 'qa/gesher-label-approval' ? APP_BOT : ACTIONS_BOT;
      const latest = w.statuses.filter(s => s.context === context &&
        s.creator?.id === writer.id && s.creator.type === 'Bot' && s.creator.login === writer.login &&
        s.url === `https://api.github.com/repos/o/r/statuses/${w.pr.head.sha}`)
        .sort((a, b) => b.id - a.id)[0];
      return latest?.state === 'success';
    });
    const approvals = w.reviews.filter(r => r.state === 'APPROVED' &&
      r.commit_id === w.pr.head.sha).length;
    if (!green || approvals < (w.nativePolicy.approvals ?? 1) ||
        !w.checks.length || !w.checks.every(c => c.status === 'completed' && c.conclusion === 'success')) return;
    w.nativeMerges.push({stage, head: w.pr.head.sha, label: w.pr.labels.length > 0,
      permission: w.permission, receiptState: w.reviews[0]?.state});
    w.pr.state = 'closed';
    w.nativeAutoMethod = null;
  };
  if (!over.runs) w.runs = ['ci', 'arm64-windows'].map((name, i) => ({
    id: 100 + i, workflow_id: 1 + i, check_suite_id: 1000 + i, name, path: '.github/workflows/' + name + '.yml',
    event: 'pull_request', status: 'completed', conclusion: 'success', run_attempt: 1, created_at: T(10),
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
      user: clone(ACTIONS_BOT), state: 'APPROVED',
      commit_id: value.head, submitted_at: T(Math.max(31, Number(event.created_at.slice(17,19)) + 1)),
      body: '<!-- gesher-automerge-v1 ' + JSON.stringify(value) + ' -->'});
  };
  if (over.approved !== false) w.seedApproval();
  w.seedOpening = (boundary = T(0), patch = {}) => {
    const value = {repository: w.pr.base.repo.id, number: w.pr.number,
      created: w.pr.created_at, boundary, ...patch};
    w.comments.push({id: 200 + w.comments.length, user: clone(ACTIONS_BOT),
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
  const statusWrite = (actor, name) => rec(name, async (args) => {
    if (w.statusErrors?.[name]) throw w.statusErrors[name];
    const value = {id: w.nextStatusId++, context: args.context, state: args.state,
      creator: clone(actor), url: `https://api.github.com/repos/${args.owner}/${args.repo}/statuses/${args.sha}`,
      ...w.statusResponseOverride?.[name]};
    w.statuses.unshift(value);
    await nativeBoundary('status', {...args, writer: clone(actor)});
    return value;
  });
  const writes = (actor, prefix = '') => ({
    pulls: {
      createReview: rec(prefix + 'approve', async (args) => {
        if (w.approveError) throw w.approveError;
        const r = {id: 400 + w.reviews.length, node_id: 'BOT_' + w.reviews.length,
          user: clone(actor), state: 'APPROVED',
          submitted_at: T(Math.max(31, Number(w.events.at(-1).created_at.slice(17,19)) + 1)), commit_id: args.commit_id, body: args.body};
        w.reviews.push(r);
        await nativeBoundary('review', args);
        return r;
      }),
      dismissReview: rec(prefix + 'dismiss', (args) => {
        const r = w.reviews.find((r) => r.id === args.review_id);
        r.state = 'DISMISSED'; return r;
      }),
    },
    issues: {
      removeLabel: rec(prefix + 'removeLabel', () => {w.pr.labels = []; return {};}),
      createComment: rec(prefix + 'comment', (args) => {
        const item = {id: 300 + w.comments.length, user: clone(actor),
          created_at: w.commentAt ?? T(31), updated_at: w.commentAt ?? T(31), body: args.body};
        w.comments.push(item); return item;
      }),
    },
  });
  const actionsWrites = writes(ACTIONS_BOT);
  const github = {rest: {
    pulls: {
      ...actionsWrites.pulls,
      get: rec('getPR', () => ({...w.pr, auto_merge: w.nativeAutoMethod ?
        {merge_method: w.nativeAutoMethod.toLowerCase()} : null})),
      list: page('listPRs', () => [{...w.pr, auto_merge: w.nativeAutoMethod ?
        {merge_method: w.nativeAutoMethod.toLowerCase()} : null}]),
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
      ...actionsWrites.issues,
      listEvents: page('events', () => w.events),
      listComments: page('comments', () => w.comments),
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
    repos: {createCommitStatus: statusWrite(ACTIONS_BOT, 'status'),
      getBranchRules: page('rules', () => {
        if (w.rulesError) throw w.rulesError;
        return w.rules ?? [];
      }),
      getCollaboratorPermissionLevel: rec('permission', () => {
        if (w.permissionError) throw w.permissionError;
        return {permission: w.permission, user: {id: w.permissionUserId ?? 42}};
      }),
      listCommitStatusesForRef: page('statuses', () => w.statuses),
      getCombinedStatusForRef: async args => {
        const response = await page('statuses', () => {
          const latest = new Map();
          for (const s of w.statuses) if (!latest.has(s.context) || latest.get(s.context).id < s.id)
            latest.set(s.context, s);
          return [...latest.values()];
        }, 'statuses')(args);
        response.data.sha = w.statusReadbackHead ?? args.ref;
        return response;
      },
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
    if (query.includes('autoMergeRequest')) return {node: {autoMergeRequest: w.nativeAutoMethod ? {mergeMethod: w.nativeAutoMethod} : null}};
    if (query.includes('enablePullRequestAutoMerge')) {
      w.calls.push(['nativeWait', clone(args)]);
      if (w.hooks.nativeWait) await w.hooks.nativeWait(args);
      if (args.head !== w.pr.head.sha) throw new Error('native auto-merge head changed');
      w.nativeAutoMethod = 'SQUASH';
      await nativeBoundary('enable', args);
      return {};
    }
    if (query.includes('dequeuePullRequest') || query.includes('disablePullRequestAutoMerge')) {
      w.calls.push(['nativeCancel', clone(args)]);
      if (w.hooks.nativeCancel) await w.hooks.nativeCancel(args);
      if (!w.nativeCancelNoop) w.nativeAutoMethod = null;
      return {};
    }
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
  w.useApp = () => {w.prGithub = {rest: {...writes(APP_BOT, 'app-'),
    repos: {createCommitStatus: statusWrite(APP_BOT, 'app-status'),
      createDispatchEvent: rec('app-dispatch', args => {
        if (w.dispatchError) throw w.dispatchError;
        w.dispatches.push({action: args.event_type, client_payload: clone(args.client_payload)});
        return {};
      })}}, graphql: github.graphql};};
  const fire = (eventName, payload) => run({github, readGithub: w.readGithub, core, prGithub: w.prGithub, mergeGithub: w.mergeGithub,
    appQaStatus: w.appQaStatus, appId: w.appId, appInstallationId: w.appInstallationId,
    repositoryId: w.envRepositoryId,
    context: {eventName, repo: {owner: 'o', repo: 'r'},
      payload: w.noRepositoryPayload ? clone(payload) :
        {repository: {default_branch: 'main', id: w.repositoryId}, ...clone(payload)}}});
  w.labeled = (snapshot = w.pr) => fire('pull_request_target', {action: 'labeled',
    label: {name: 'automerge'}, sender: {id: 42}, pull_request: snapshot});
  w.pushed = (snapshot = w.pr) => fire('pull_request_target', {action: 'synchronize', pull_request: snapshot});
  w.drainDispatches = async () => {
    while (w.dispatches.length) await fire('repository_dispatch', w.dispatches.shift());
  };
  w.ciDone = async (sha = HEAD, sourceRepo = w.pr.head.repo.id) => {
    await fire('workflow_run', {
      workflow_run: {event: 'pull_request', head_sha: sha, head_repository: {id: sourceRepo}}});
    await w.drainDispatches();
  };
  // Convenience helpers drain separate queued jobs. Tests of queue boundaries
  // use fire() and drainDispatches() explicitly so enqueueing cannot mutate QA.
  w.sweep = async () => {await fire('schedule', {}); await w.drainDispatches();};
  w.fire = fire;
  w.github = github;
  w.did = (name) => w.calls.filter((c) => c[0] === name);
  return w;
}

function authorWorld(over = {}) {
  const w = world(over);
  w.pr.user = {id: 42, login: 'maintainer'};
  return w;
}


module.exports = {world, authorWorld, HEAD, OTHER, BASE, T, APP_BOT};
