'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const publish = require('./app-required-checks.js');
const {APP_ID, INSTALLATION_ID} = require('./app-qa-status.js');
const {APP_SLUG} = require('./merge-approval.js');
const HEAD = 'a'.repeat(40);
const clone = structuredClone;

function fixture() {
  const w = {calls: [], pr: {number: 7, state: 'open', auto_merge: null,
    base: {ref: 'main', repo: {id: 10}}, head: {sha: HEAD, ref: 'topic', repo: {id: 20}}},
    workflow: {id: 100, state: 'active', path: '.github/workflows/ci.yml'},
    run: {id: 200, run_attempt: 2, workflow_id: 100, path: '.github/workflows/ci.yml',
      event: 'pull_request', head_sha: HEAD, head_branch: 'topic', repository: {id: 10},
      head_repository: {id: 20}, check_suite_id: 300, status: 'completed', conclusion: 'success'},
    job: {id: 400, run_id: 200, head_sha: HEAD, name: 'ci-required', status: 'completed',
      conclusion: 'success', check_run_url: 'https://api.github.com/repos/o/r/check-runs/500'},
    check: {id: 500, name: 'ci-required', head_sha: HEAD, app: {id: 15368},
      check_suite: {id: 300}, status: 'completed', conclusion: 'success'}};
  const page = (field, fn) => async args => ({data: {[field]: fn(args)}});
  const github = {rest: {
    pulls: {get: async () => ({data: clone(w.pr)}), list: async () => ({data: [clone(w.pr)]})},
    actions: {
      getWorkflow: async args => ({data: {...w.workflow, path: '.github/workflows/' +
        (typeof args.workflow_id === 'string' ? args.workflow_id : 'ci.yml')}}),
      listWorkflowRuns: page('workflow_runs', () => w.missing ? [] : [clone(w.run)]),
      getWorkflowRun: async () => {
        w.reads = (w.reads || 0) + 1;
        if (w.onRunRead) w.onRunRead(w.reads);
        return {data: clone(w.run)};
      },
      listJobsForWorkflowRunAttempt: page('jobs', args => {
        assert.equal(args.attempt_number, w.run.run_attempt);
        return w.jobs || [clone(w.job)];
      }),
    },
    checks: {get: async () => ({data: clone(w.check)})},
    repos: {createCommitStatus: async args => {w.calls.push(clone(args)); return {data: args};}},
  }};
  w.github = github;
  w.context = {repo: {owner: 'o', repo: 'r'}, payload: {inputs: {pull_request_number: '7'}}};
  w.args = {github, context: w.context, appId: APP_ID, installationId: INSTALLATION_ID,
    appSlug: APP_SLUG, core: {warning: () => {}}};
  w.evidence = () => publish.evidence(github, 'o', 'r', w.pr, publish.SOURCES[0]);
  w.publish = () => publish(w.args);
  return w;
}

test('real current workflow and its aggregate check supply the App result', async () => {
  const w = fixture();
  const result = await w.evidence();
  assert.equal(result.state, 'success');
  assert.equal(result.target_url, 'https://github.com/o/r/actions/runs/200/attempts/2');
});

test('both published contexts require their own canonical aggregate job', async () => {
  for (const source of publish.SOURCES) {
    const w = fixture();
    w.run.path = '.github/workflows/' + source.file;
    w.job.name = w.check.name = source.job;
    assert.equal((await publish.evidence(w.github, 'o', 'r', w.pr, source)).state, 'success');
  }
});

test('a push during verification cannot publish success for the new head', async () => {
  const w = fixture();
  w.onRunRead = () => {w.pr.head.sha = 'b'.repeat(40);};
  await w.publish();
  assert.equal(w.calls.filter(c => c.state === 'success').length, 0);
  assert.ok(w.calls.every(c => c.sha === HEAD));
});

for (const [name, alter] of Object.entries({
  'another head': w => {w.run.head_sha = 'b'.repeat(40);},
  'another fork': w => {w.run.head_repository.id = 999;},
  'another target repository': w => {w.run.repository.id = 999;},
  'another branch': w => {w.run.head_branch = 'other';},
  'manual run': w => {w.run.event = 'workflow_dispatch';},
  'another workflow path': w => {w.run.path = '.github/workflows/other.yml';},
})) {
  test(name + ' cannot supply CI success', async () => {
    const w = fixture(); alter(w);
    assert.equal((await w.evidence()).state, 'pending');
  });
}

for (const [name, alter] of Object.entries({
  'disabled workflow': w => {w.workflow.state = 'disabled_manually';},
  'missing aggregate': w => {w.jobs = [];},
  'ambiguous aggregate': w => {w.jobs = [w.job, w.job];},
  'foreign check publisher': w => {w.check.app.id = 5816428;},
  'old check suite': w => {w.check.check_suite.id = 299;},
  'job from another run': w => {w.job.run_id = 199;},
  'job from another head': w => {w.job.head_sha = 'b'.repeat(40);},
  'foreign check URL': w => {w.job.check_run_url = 'https://example.org/check-runs/500';},
})) {
  test(name + ' fails closed', async () => {
    const w = fixture(); alter(w);
    await assert.rejects(w.evidence());
  });
}

for (const conclusion of ['failure', 'cancelled', 'timed_out', 'skipped', 'neutral']) {
  test('aggregate ' + conclusion + ' never becomes success', async () => {
    const w = fixture(); w.job.conclusion = conclusion;
    assert.equal((await w.evidence()).state, 'failure');
  });
}

test('queued reruns and workflow approval remain pending', async () => {
  const w = fixture(); w.run.status = 'queued';
  assert.equal((await w.evidence()).state, 'pending');
  w.run.status = 'completed'; w.run.conclusion = 'action_required';
  assert.equal((await w.evidence()).state, 'pending');
});

test('a failed workflow cannot be hidden by a green aggregate', async () => {
  const w = fixture(); w.run.conclusion = 'failure';
  assert.equal((await w.evidence()).state, 'failure');
});

test('publishes pending before checking evidence; changed attempts never publish success', async () => {
  const w = fixture();
  w.onRunRead = n => {if (n === 2) w.run.run_attempt++;};
  await w.publish();
  assert.equal(w.calls[0].state, 'pending');
  assert.equal(w.calls.filter(c => c.state === 'success').length, 0);
});

test('a rerun observed after success revokes it to pending', async () => {
  const w = fixture();
  w.onRunRead = n => {if (n === 3) {w.run.run_attempt++; w.run.status = 'in_progress';}};
  await w.publish();
  const ci = w.calls.filter(c => c.context === 'ci/gesher-required');
  assert.deepEqual(ci.map(c => c.state), ['pending', 'success', 'pending']);
});

test('API errors revoke old green with pending and fail the publisher', async () => {
  const w = fixture();
  w.github.rest.actions.getWorkflow = async () => {throw new Error('API unavailable');};
  await assert.rejects(w.publish());
  assert.deepEqual(w.calls.map(c => c.state), ['pending', 'pending']);
});

test('an API error after publication revokes the new success', async () => {
  const w = fixture();
  w.onRunRead = n => {if (n === 3) throw new Error('readback unavailable');};
  await assert.rejects(w.publish());
  const ci = w.calls.filter(c => c.context === 'ci/gesher-required');
  assert.deepEqual(ci.map(c => c.state), ['pending', 'success', 'pending']);
});

test('does not write advisory statuses into inherited native waiting', async () => {
  const w = fixture(); w.pr.auto_merge = {merge_method: 'squash'};
  await w.publish();
  assert.equal(w.calls.length, 0);
});

test('fork completions resolve through current PR metadata without trusting PR arrays', async () => {
  const w = fixture();
  w.context.payload = {workflow_run: {event: 'pull_request', head_sha: 'b'.repeat(40),
    head_repository: {id: 20}, pull_requests: [{number: 7}]}};
  await w.publish();
  assert.equal(w.calls.length, 0);
});

test('requires the exact installed App before any writes', async () => {
  const w = fixture(); w.args.installationId++;
  await assert.rejects(w.publish(), /verified Gesher App/);
  assert.equal(w.calls.length, 0);
});
