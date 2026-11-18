'use strict';

// Only loaded from main. Approve the exact head and let native auto-merge finish.
const p = require('./merge-approval.js');
const STATUS = 'qa/label-approval';
// Fork CI awaiting maintainer approval can be completed/action_required without
// executing a job. Keep approval pending until an actual CI result is available.
const FAILED_CI = new Set(['failure', 'timed_out', 'startup_failure']);

async function approved(github, owner, repo, pr) {
  if (pr.state !== 'open' || pr.draft || pr.base.ref !== 'main' || !p.hasLabel(pr) ||
      !p.SHA.test(pr.head.sha)) return false;
  const event = await p.labelEvent(github, owner, repo, pr.number);
  if (!event || event.invalidated || !event.unambiguous ||
      !await p.authorized(github, owner, repo, event)) return false;
  const boundary = await p.openingBoundary(github, owner, repo, pr);
  const qa = await p.authorQa(github, owner, repo, pr, event, boundary);
  if (!p.explicitApplication(pr, event, boundary, qa) ||
      !await p.approval(github, owner, repo, pr, event)) return false;
  const {data: current} = await github.rest.pulls.get({owner, repo, pull_number: pr.number});
  const latest = await p.labelEvent(github, owner, repo, pr.number);
  return p.sameTarget(pr, current) && current.state === 'open' && !current.draft &&
    p.hasLabel(current) && latest?.id === event.id && !latest.invalidated && latest.unambiguous;
}

async function status(github, owner, repo, sha, ok) {
  await github.rest.repos.createCommitStatus({owner, repo, sha, context: STATUS,
    state: ok === null ? 'pending' : ok ? 'success' : 'failure', description: ok === null ?
      'Authorized label recorded; waiting for required PR CI' : ok ?
      'Authorized exact-head label, QA and PR CI verified' : 'Fresh authorized automerge label/QA required'});
}

async function cancel(github, owner, repo, pr, mergeGithub = github) {
  const {data: current} = await github.rest.pulls.get({owner, repo, pull_number: pr.number});
  if (current.state !== 'open' || current.head.sha !== pr.head.sha) return;
  const {node} = await github.graphql(`query($id: ID!) {
    node(id: $id) { ... on PullRequest { autoMergeRequest { enabledAt } } }
  }`, {id: pr.node_id});
  if (node?.autoMergeRequest) await mergeGithub.graphql(`mutation($id: ID!) {
    disablePullRequestAutoMerge(input: {pullRequestId: $id}) { clientMutationId }
  }`, {id: pr.node_id});
}

async function ciEvidence(github, owner, repo, pr) {
  const runs = await p.list(github.rest.actions.listWorkflowRunsForRepo,
    {owner, repo, head_sha: pr.head.sha}, 'workflow_runs');
  const checks = await p.list(github.rest.checks.listForRef,
    {owner, repo, ref: pr.head.sha, filter: 'latest'}, 'check_runs');
  let ready = true;
  for (const [file, name] of [['ci.yml', 'ci-required'], ['arm64-windows.yml', 'arm64-required']]) {
    const {data: workflow} = await github.rest.actions.getWorkflow({owner, repo, workflow_id: file});
    if (!Number.isSafeInteger(workflow.id) || workflow.id <= 0 ||
        workflow.state !== 'active' || workflow.path !== '.github/workflows/' + file) return {ready: false};
    const matching = runs.filter(r => r.workflow_id === workflow.id && r.path === workflow.path &&
      r.event === 'pull_request' && r.head_sha === pr.head.sha && r.head_branch === pr.head.ref &&
      r.repository?.id === pr.base.repo.id && r.head_repository?.id === pr.head.repo.id);
    const listed = matching.sort((a, b) => b.id - a.id)[0];
    if (!listed || !Number.isSafeInteger(listed.id) || listed.id <= 0) { ready = false; continue; }
    // A rerun keeps its run ID. Read its current attempt rather than reacting
    // to a delayed failure event from the previous attempt.
    const {data: latest} = await github.rest.actions.getWorkflowRun({owner, repo, run_id: listed.id});
    if (latest.id !== listed.id || !Number.isSafeInteger(latest.run_attempt) || latest.run_attempt <= 0 ||
        latest.workflow_id !== workflow.id || latest.path !== workflow.path ||
        latest.event !== 'pull_request' || latest.head_sha !== pr.head.sha ||
        latest.head_branch !== pr.head.ref || latest.repository?.id !== pr.base.repo.id ||
        latest.head_repository?.id !== pr.head.repo.id || !Number.isSafeInteger(latest.check_suite_id) ||
        latest.check_suite_id <= 0) return {ready: false};
    if (latest.status === 'completed' && FAILED_CI.has(latest.conclusion)) return {ready: false, failed: latest};
    if (latest.status !== 'completed' || latest.conclusion !== 'success') ready = false;
    const gate = checks.filter(c => c.name === name && c.app?.id === 15368 &&
      c.check_suite?.id === latest.check_suite_id).sort((a, b) => b.id - a.id)[0];
    if (!gate || !Number.isSafeInteger(gate.id) || gate.id <= 0 ||
        gate.status !== 'completed' || gate.conclusion !== 'success') ready = false;
  }
  return {ready};
}

async function ciReady(github, owner, repo, pr) {
  return (await ciEvidence(github, owner, repo, pr)).ready;
}

async function failedCI(github, owner, repo, pr, event, run, core, mergeGithub) {
  if (!event) return;
  // Recheck attempt state immediately before removing approval. An old failure
  // must not revoke a newer successful/pending rerun or a later label event.
  const {data: currentRun} = await github.rest.actions.getWorkflowRun({owner, repo, run_id: run.id});
  if (currentRun.head_sha !== pr.head.sha || currentRun.head_repository?.id !== pr.head.repo.id ||
      currentRun.repository?.id !== pr.base.repo.id || currentRun.status !== 'completed' ||
      !FAILED_CI.has(currentRun.conclusion) || currentRun.run_attempt !== run.run_attempt) return;
  const removed = await p.removeLabel(github, owner, repo, pr, event, 'CI failed; fresh approval required', core);
  if (!removed) return;
  await status(github, owner, repo, pr.head.sha, false);
  await cancel(github, owner, repo, pr, mergeGithub);
  const marker = '<!-- gesher-automerge-ci-failed-v1 ' + JSON.stringify({
    repository: pr.base.repo.id, number: pr.number, head: pr.head.sha,
    label: event.id, run: run.id, attempt: run.run_attempt}) + ' -->';
  const comments = await p.list(github.rest.issues.listComments, {owner, repo, issue_number: pr.number});
  if (comments.some(c => c.user?.login === 'github-actions[bot]' && c.user.type === 'Bot' && c.body?.includes(marker))) return;
  const link = `https://github.com/${owner}/${repo}/actions/runs/${run.id}`;
  await github.rest.issues.createComment({owner, repo, issue_number: pr.number,
    body: 'CI failed; removed `automerge`. Fix/rerun CI, then reapply the label. [Run](' + link + ').\n\n' + marker});
}

async function reconcile(github, owner, repo, number, core, mergeGithub) {
  const {data: pr} = await github.rest.pulls.get({owner, repo, pull_number: number});
  if (pr.state !== 'open') return;
  let ok = false;
  try { ok = await approved(github, owner, repo, pr); }
  catch (error) {
    await status(github, owner, repo, pr.head.sha, false);
    await cancel(github, owner, repo, pr, mergeGithub);
    throw error;
  }
  if (!ok) {
    await status(github, owner, repo, pr.head.sha, false);
    return cancel(github, owner, repo, pr, mergeGithub);
  }
  // This gate stays pending until CI is done. In particular fork review events
  // have read-only tokens; the privileged CI-completion event rechecks QA then.
  await status(github, owner, repo, pr.head.sha, null);
  const {node} = await github.graphql(`query($id: ID!) {
    node(id: $id) { ... on PullRequest { autoMergeRequest { mergeMethod } } }
  }`, {id: pr.node_id});
  if (node?.autoMergeRequest?.mergeMethod !== 'SQUASH') {
    if (node?.autoMergeRequest) await cancel(github, owner, repo, pr, mergeGithub);
    await mergeGithub.graphql(`mutation($id: ID!, $head: GitObjectID!) {
      enablePullRequestAutoMerge(input: {pullRequestId: $id, expectedHeadOid: $head,
        mergeMethod: SQUASH}) { clientMutationId }
    }`, {id: pr.node_id, head: pr.head.sha});
  }
  const event = await p.labelEvent(github, owner, repo, number);
  const evidence = await ciEvidence(github, owner, repo, pr);
  if (evidence.failed) return failedCI(github, owner, repo, pr, event, evidence.failed, core, mergeGithub);
  if (!evidence.ready) return;
  if (!await approved(github, owner, repo, pr)) {
    await status(github, owner, repo, pr.head.sha, false);
    return cancel(github, owner, repo, pr, mergeGithub);
  }
  await status(github, owner, repo, pr.head.sha, true);
}

// Only native auto-merge mutations use the optional workflow-capable identity.
// Receipts, permission checks and required statuses retain github-actions[bot].
module.exports = async ({github, context, core, mergeGithub = github}) => {
  const {owner, repo} = context.repo;
  const payload = context.payload;
  if (context.eventName === 'workflow_run') {
    const run = payload.workflow_run;
    if (run.event !== 'pull_request' || !p.SHA.test(run.head_sha) || !run.head_repository?.id) return;
    const prs = await p.list(github.rest.pulls.list, {owner, repo, state: 'open', base: 'main'});
    for (const pr of prs.filter(x => x.head.sha === run.head_sha && x.head.repo?.id === run.head_repository.id)) {
      await reconcile(github, owner, repo, pr.number, core, mergeGithub);
    }
    return;
  }
  if (context.eventName === 'pull_request_review' &&
      payload.pull_request?.head?.repo?.id !== payload.pull_request?.base?.repo?.id) return;
  if (context.eventName === 'workflow_dispatch') {
    const requested = payload.inputs?.pull_request_number;
    if (requested) {
      if (!/^[1-9][0-9]*$/.test(requested) || !Number.isSafeInteger(Number(requested)))
        throw new Error('Invalid pull request number');
      return reconcile(github, owner, repo, Number(requested), core, mergeGithub);
    }
    const prs = await p.list(github.rest.pulls.list, {owner, repo, state: 'open', base: 'main'});
    for (const pr of prs) {
      try { await reconcile(github, owner, repo, pr.number, core, mergeGithub); }
      catch (error) { core.warning('#' + pr.number + ': ' + error.message); }
    }
    return;
  }
  const number = payload.pull_request?.number;
  if (!number) return;
  if (context.eventName === 'pull_request_target') {
    if (['labeled', 'unlabeled'].includes(payload.action) && payload.label?.name !== 'automerge') return;
    const {data: pr} = await github.rest.pulls.get({owner, repo, pull_number: number});
    if (pr.state !== 'open') return;
    if (payload.action === 'synchronize') {
      await p.dismissStaleApprovals(github, owner, repo, pr);
      // A delayed push must never remove a fresh opt-in for a newer head.
      if (p.sameTarget(pr, payload.pull_request)) {
        const event = await p.labelEvent(github, owner, repo, number);
        if (event && p.hasLabel(pr)) {
          const receipt = await p.approval(github, owner, repo, pr, event);
          const applied = Date.parse(event.created_at);
          const pushed = Date.parse(payload.pull_request.updated_at);
          // Receipts bind a label to its approved head. Without a receipt,
          // retain same/adjacent-second labels: webhook timestamps cannot prove
          // that a newly reapplied label preceded the push in that interval.
          const olderPendingLabel = receipt === null && Number.isFinite(applied) &&
            Number.isFinite(pushed) && applied + 1000 < pushed;
          if (event.invalidated || receipt === false || olderPendingLabel)
            await p.removeLabel(github, owner, repo, pr, event, 'new push requires fresh approval', core);
        }
      }
    }
    if (payload.action === 'labeled') {
      const event = await p.labelEvent(github, owner, repo, number, payload.pull_request.updated_at);
      if (event) {
        const boundary = await p.openingBoundary(github, owner, repo, pr);
        const qa = await p.authorQa(github, owner, repo, pr, event, boundary);
        if (!p.explicitApplication(pr, event, boundary, qa) || event.invalidated) {
          await p.removeLabel(github, owner, repo, pr, event, 'fresh label/QA required', core);
        } else if (p.sameTarget(pr, payload.pull_request) && event.unambiguous &&
            event.deliveryMatches && event.actor?.id === payload.sender?.id &&
            await p.authorized(github, owner, repo, event)) {
          const receipt = await p.approval(github, owner, repo, pr, event);
          if (receipt === null) await github.rest.pulls.createReview({owner, repo, pull_number: number,
            commit_id: pr.head.sha, event: 'APPROVE',
            body: '<!-- gesher-automerge-v1 ' + JSON.stringify(p.approvalFor(pr, event)) + ' -->'});
        }
      }
    }
  }
  await reconcile(github, owner, repo, number, core, mergeGithub);
};
module.exports.approved = approved;
module.exports.ciReady = ciReady;
module.exports.STATUS = STATUS;
