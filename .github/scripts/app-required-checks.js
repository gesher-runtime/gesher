'use strict';

// Trusted metadata bridge: never loads PR code, artifacts or logs.
const {list, SHA, APP_SLUG} = require('./merge-approval.js');
const {APP_ID, INSTALLATION_ID} = require('./app-qa-status.js');
const SOURCES = [
  {file: 'ci.yml', job: 'ci-required', context: 'ci/gesher-required'},
  {file: 'arm64-windows.yml', job: 'arm64-required', context: 'arm64/gesher-required'},
];
const positive = n => Number.isSafeInteger(n) && n > 0;
const pending = description => ({state: 'pending', description});

async function evidence(github, owner, repo, pr, source) {
  const {data: workflow} = await github.rest.actions.getWorkflow({owner, repo, workflow_id: source.file});
  const path = '.github/workflows/' + source.file;
  if (!positive(workflow.id) || workflow.path !== path || workflow.state !== 'active')
    throw new Error('Required workflow registration is unavailable: ' + source.file);
  const matches = run => run.workflow_id === workflow.id && run.path === path &&
    run.event === 'pull_request' && run.head_sha === pr.head.sha &&
    run.head_branch === pr.head.ref && run.repository?.id === pr.base.repo.id &&
    run.head_repository?.id === pr.head.repo.id;
  const runs = await list(github.rest.actions.listWorkflowRuns, {owner, repo,
    workflow_id: workflow.id, head_sha: pr.head.sha, event: 'pull_request'}, 'workflow_runs');
  const candidate = runs.filter(matches).sort((a, b) => b.id - a.id)[0];
  if (!candidate) return {...pending('Waiting for the pull request workflow'), fingerprint: 'missing'};
  if (!positive(candidate.id)) throw new Error('Invalid workflow run identity');
  const {data: run} = await github.rest.actions.getWorkflowRun({owner, repo, run_id: candidate.id});
  if (run.id !== candidate.id || !matches(run) || !positive(run.run_attempt) || !positive(run.check_suite_id))
    throw new Error('Required workflow run identity changed');
  const result = {fingerprint: JSON.stringify([workflow.id, run.id, run.run_attempt, run.status,
    run.conclusion, run.check_suite_id]),
    target_url: `https://github.com/${owner}/${repo}/actions/runs/${run.id}/attempts/${run.run_attempt}`};
  if (run.status !== 'completed' || run.conclusion === 'action_required')
    return {...result, ...pending('Waiting for the current workflow attempt')};
  if (run.conclusion !== 'success')
    return {...result, state: 'failure', description: 'The current workflow attempt did not succeed'};
  // Listing all attempt jobs can return HTTP 502 even for successful runs. Locate the
  // named aggregate in this run's check suite, then read only its individual
  // job. A successful check from an older attempt is never sufficient evidence.
  const checks = await list(github.rest.checks.listForSuite, {owner, repo,
    check_suite_id: run.check_suite_id, check_name: source.job, filter: 'latest'}, 'check_runs');
  const gates = checks.filter(check => check.name === source.job);
  if (gates.length !== 1) throw new Error('Required aggregate check is missing or ambiguous');
  const gate = gates[0];
  const validCheck = check => positive(check?.id) && check.id === gate.id &&
    check.name === source.job && check.app?.id === 15368 &&
    check.head_sha === pr.head.sha && check.check_suite?.id === run.check_suite_id;
  if (!validCheck(gate)) throw new Error('Required aggregate check identity mismatch');
  const prefix = `https://github.com/${owner}/${repo}/actions/runs/${run.id}/job/`;
  const suffix = gate.details_url?.startsWith(prefix) ? gate.details_url.slice(prefix.length) : '';
  const jobId = /^[1-9]\d*$/.test(suffix) ? Number(suffix) : NaN;
  if (!positive(jobId)) throw new Error('Required aggregate check has an invalid job link');
  const {data: job} = await github.rest.actions.getJobForWorkflowRun({owner, repo, job_id: jobId});
  if (job.id !== jobId || job.name !== source.job || job.run_id !== run.id ||
      job.run_attempt !== run.run_attempt || job.head_sha !== pr.head.sha ||
      job.check_run_url !== `https://api.github.com/repos/${owner}/${repo}/check-runs/${gate.id}`)
    throw new Error('Required aggregate job does not belong to this run/attempt/head/check');
  const {data: check} = await github.rest.checks.get({owner, repo, check_run_id: gate.id});
  if (!validCheck(check) || check.details_url !== gate.details_url)
    throw new Error('Required aggregate check identity mismatch');
  result.fingerprint += JSON.stringify([job.id, job.run_attempt, job.status, job.conclusion,
    check.id, check.status, check.conclusion]);
  const success = job.status === 'completed' && job.conclusion === 'success' &&
    check.status === 'completed' && check.conclusion === 'success';
  return {...result, state: success ? 'success' : 'failure',
    description: success ? 'Current workflow and aggregate job succeeded' : 'Required aggregate job did not succeed'};
}

module.exports = async ({github, context, core, appId, installationId, appSlug}) => {
  if (appId !== APP_ID || installationId !== INSTALLATION_ID || appSlug !== APP_SLUG)
    throw new Error('Required checks need the verified Gesher App installation');
  const {owner, repo} = context.repo;
  const payload = context.payload;
  const requested = payload.inputs?.pull_request_number;
  if (requested && (!/^[1-9][0-9]*$/.test(requested) || !positive(Number(requested))))
    throw new Error('Invalid pull request number');
  const number = requested ? Number(requested) : payload.pull_request?.number;
  const prs = number ? [{number}] : await list(github.rest.pulls.list, {owner, repo, state: 'open', base: 'main'});
  const write = (sha, source, value) => github.rest.repos.createCommitStatus({owner, repo, sha,
    context: source.context, state: value.state, description: value.description,
    ...(value.target_url ? {target_url: value.target_url} : {})});
  const same = (a, b) => b.state === 'open' && b.base.ref === 'main' && !b.auto_merge &&
    a.head.sha === b.head.sha && a.head.repo.id === b.head.repo.id && a.head.ref === b.head.ref;
  const errors = [];
  for (const item of prs) {
    const {data: pr} = await github.rest.pulls.get({owner, repo, pull_number: item.number});
    if (pr.state !== 'open' || pr.base.ref !== 'main' || !SHA.test(pr.head.sha)) continue;
    const eventRun = payload.workflow_run;
    if (eventRun && (eventRun.event !== 'pull_request' || eventRun.head_sha !== pr.head.sha ||
        eventRun.head_repository?.id !== pr.head.repo.id)) continue;
    // An unrelated status write must not wake an inherited native request.
    if (pr.auto_merge) {
      core.warning('#' + pr.number + ': native waiting must be reconciled before bot CI publication');
      continue;
    }
    for (const source of SOURCES) {
      let publishedSuccess = false;
      try {
        await write(pr.head.sha, source, pending('Verifying current CI evidence'));
        const value = await evidence(github, owner, repo, pr, source);
        const {data: current} = await github.rest.pulls.get({owner, repo, pull_number: pr.number});
        if (!same(pr, current)) continue;
        // A delayed completion must not publish an old attempt's success.
        const fresh = await evidence(github, owner, repo, pr, source);
        if (value.fingerprint !== fresh.fingerprint) continue;
        await write(pr.head.sha, source, value);
        publishedSuccess = value.state === 'success';
        if (value.state === 'success') {
          const after = await evidence(github, owner, repo, pr, source);
          if (value.fingerprint !== after.fingerprint)
            await write(pr.head.sha, source, pending('CI changed during publication; awaiting reconciliation'));
        }
      } catch (error) {
        errors.push(error);
        core.warning('#' + pr.number + ' ' + source.context + ': ' + error.message);
        if (publishedSuccess) {
          try { await write(pr.head.sha, source, pending('CI readback unavailable; awaiting reconciliation')); }
          catch (revokeError) {
            core.warning('#' + pr.number + ' CI status revocation failed: ' + revokeError.message);
          }
        }
      }
    }
  }
  if (errors.length) throw new Error('Required CI evidence could not be verified (' + errors.length + ' errors)');
};
module.exports.evidence = evidence;
module.exports.SOURCES = SOURCES;
