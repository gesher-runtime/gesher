'use strict';

// Only loaded from main. Release an approved exact head through GitHub policy.
const p = require('./merge-approval.js');
const qaStatuses = require('./app-qa-status.js');
const {STATUS} = qaStatuses;
const RECONCILE_EVENT = 'gesher-automerge-reconcile';
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

async function cancel(github, owner, repo, pr, mergeGithub = github) {
  const {data: current} = await github.rest.pulls.get({owner, repo, pull_number: pr.number});
  if (current.state !== 'open' || current.head.sha !== pr.head.sha) return;
  // The REST pull request already carries native auto-merge state.
  if (current.auto_merge) await mergeGithub.graphql(`mutation($id: ID!) {
    disablePullRequestAutoMerge(input: {pullRequestId: $id}) { clientMutationId }
  }`, {id: pr.node_id});
}

async function disarm(github, mergeGithub, owner, repo, pr) {
  let {data: current} = await github.rest.pulls.get({owner, repo, pull_number: pr.number});
  if (current.state !== 'open' || !p.sameTarget(pr, current)) return false;
  if (current.auto_merge) {
    await mergeGithub.graphql(`mutation($id: ID!) {
      disablePullRequestAutoMerge(input: {pullRequestId: $id}) { clientMutationId }
    }`, {id: pr.node_id});
    ({data: current} = await github.rest.pulls.get({owner, repo, pull_number: pr.number}));
    if (current.state !== 'open' || !p.sameTarget(pr, current)) return false;
    if (current.auto_merge) throw new Error('Native auto-merge remained armed during QA publication');
  }
  return true;
}

async function enable(mergeGithub, pr) {
  await mergeGithub.graphql(`mutation($id: ID!, $head: GitObjectID!) {
    enablePullRequestAutoMerge(input: {pullRequestId: $id, expectedHeadOid: $head,
      mergeMethod: SQUASH}) { clientMutationId }
  }`, {id: pr.node_id, head: pr.head.sha});
}

async function nativeReleaseState(github, pr) {
  const {node} = await github.graphql(`query($id: ID!) {
    node(id: $id) { ... on PullRequest {
      id state isDraft headRefOid baseRefName mergeable mergeStateStatus
    } }
  }`, {id: pr.node_id});
  if (node?.id !== pr.node_id || node.state !== 'OPEN' || node.isDraft !== false ||
      node.headRefOid !== pr.head.sha || node.baseRefName !== pr.base.ref ||
      node.mergeable !== 'MERGEABLE') throw new Error('Native merge state or reviewed target is unavailable');
  // GitHub CLI uses the immediate path for these states: auto-merge enable
  // rejects a CLEAN PR. Both supported mutations still enforce GitHub policy.
  if (['CLEAN', 'HAS_HOOKS', 'UNSTABLE'].includes(node.mergeStateStatus)) return 'merge';
  if (['BLOCKED', 'BEHIND'].includes(node.mergeStateStatus)) return 'wait';
  throw new Error('Native merge state cannot be released: ' + node.mergeStateStatus);
}

async function release(mergeGithub, pr, mode) {
  if (mode === 'wait') return enable(mergeGithub, pr);
  await mergeGithub.graphql(`mutation($id: ID!, $head: GitObjectID!) {
    mergePullRequest(input: {pullRequestId: $id, expectedHeadOid: $head,
      mergeMethod: SQUASH}) { clientMutationId }
  }`, {id: pr.node_id, head: pr.head.sha});
}

async function invalidate(github, mergeGithub, owner, repo, pr, statuses, core) {
  // Read back an ambiguous mutation before any cleanup write. Never substitute
  // another release mutation, and never alter a newer or already closed PR.
  let current;
  try { ({data: current} = await github.rest.pulls.get({owner, repo, pull_number: pr.number})); }
  catch (error) {
    core.warning('#' + pr.number + ': release state unavailable; cleanup deferred: ' + error.message);
    return;
  }
  if (current.state !== 'open' || !p.sameTarget(pr, current)) return;
  let nativeOff = false;
  try {
    nativeOff = await disarm(github, mergeGithub, owner, repo, pr);
  } catch (error) { core.warning('#' + pr.number + ': native disarming unconfirmed: ' + error.message); }
  try { await statuses.set(pr.head.sha, false, {nativeOff}); }
  catch (error) { core.warning('#' + pr.number + ': QA revocation unconfirmed: ' + error.message); }
}

async function ciEvidence(github, owner, repo, pr) {
  const runs = await p.list(github.rest.actions.listWorkflowRunsForRepo,
    {owner, repo, head_sha: pr.head.sha}, 'workflow_runs');
  const checks = await p.list(github.rest.checks.listForRef,
    {owner, repo, ref: pr.head.sha, filter: 'latest'}, 'check_runs');
  let ready = true;
  for (const [file, name] of [['ci.yml', 'ci-required'], ['arm64-windows.yml', 'arm64-required']]) {
    // Registration can change while publishing the App status. Every evidence
    // pass must revalidate its current identity, path and enabled state.
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

async function failedCI(github, prGithub, owner, repo, pr, event, run, core, mergeGithub, statuses) {
  if (!event) return;
  // Recheck attempt state immediately before removing approval. An old failure
  // must not revoke a newer successful/pending rerun or a later label event.
  const {data: currentRun} = await github.rest.actions.getWorkflowRun({owner, repo, run_id: run.id});
  if (currentRun.head_sha !== pr.head.sha || currentRun.head_repository?.id !== pr.head.repo.id ||
      currentRun.repository?.id !== pr.base.repo.id || currentRun.status !== 'completed' ||
      !FAILED_CI.has(currentRun.conclusion) || currentRun.run_attempt !== run.run_attempt) return;
  const removed = await p.removeLabel(github, prGithub, owner, repo, pr, event, 'CI failed; fresh approval required', core);
  if (!removed) return;
  await statuses.set(pr.head.sha, false);
  await cancel(github, owner, repo, pr, mergeGithub);
  const marker = '<!-- gesher-automerge-ci-failed-v1 ' + JSON.stringify({
    repository: pr.base.repo.id, number: pr.number, head: pr.head.sha,
    label: event.id, run: run.id, attempt: run.run_attempt}) + ' -->';
  const comments = await p.list(github.rest.issues.listComments, {owner, repo, issue_number: pr.number});
  if (comments.some(c => p.isReceiptBot(c.user) && c.body?.includes(marker))) return;
  const link = `https://github.com/${owner}/${repo}/actions/runs/${run.id}`;
  await prGithub.rest.issues.createComment({owner, repo, issue_number: pr.number,
    body: 'CI failed; removed `automerge`. Fix/rerun CI, then reapply the label. [Run](' + link + ').\n\n' + marker});
}

// Pending/failed gates need recovery after CI completes. A green gate is only
// settled while its writer, exact head, current CI and approval remain valid.
// In particular a same-head rerun can invalidate an earlier trusted success.
async function currentGate(statusReader, owner, repo, pr, gate) {
  const values = await p.list(async args => {
    const response = await statusReader.rest.repos.getCombinedStatusForRef(args);
    if (response.data.sha !== pr.head.sha) throw new Error('QA recovery status head mismatch');
    return response;
  }, {owner, repo, ref: pr.head.sha}, 'statuses');
  const matching = values.filter(s => s.context === gate);
  const latest = matching.sort((a, b) => b.id - a.id)[0];
  return {matching, latest};
}

async function stuck(github, statusReader, owner, repo, pr, gate) {
  const {matching, latest} = await currentGate(statusReader, owner, repo, pr, gate);
  const evidence = await ciEvidence(github, owner, repo, pr);
  if (latest?.state === 'success') {
    if (matching.some(s => !Number.isSafeInteger(s.id) || s.id <= 0) ||
        !qaStatuses.trustedStatus(latest, owner, repo, pr.head.sha, gate, 'success') ||
        !evidence.ready) return true;
    if (!await approved(github, owner, repo, pr)) return true;
    // A previous controller can stop after publishing success but before its
    // final native enable. Green alone must not strand that recoverable PR.
    const {data: current} = await github.rest.pulls.get({owner, repo, pull_number: pr.number});
    return !p.sameTarget(pr, current) || current.state !== 'open' || current.draft ||
      !p.hasLabel(current) || current.auto_merge?.merge_method !== 'squash';
  }
  return evidence.ready || Boolean(evidence.failed);
}

async function reconcile(github, prGithub, owner, repo, number, core, mergeGithub, publisher, expectedHead) {
  const {data: pr} = await github.rest.pulls.get({owner, repo, pull_number: number});
  if (pr.state !== 'open' || (expectedHead && pr.head.sha !== expectedHead)) return;
  let nativeOff = false;
  const statuses = {...publisher, set: (sha, ok) => publisher.set(sha, ok, {nativeOff})};
  let approvalChecked = false;
  try {
    // An old green gate may already have native waiting armed. Disarm before
    // any status write, including revocation of an invalid approval: even a
    // legacy status write can wake native evaluation of the existing App gate.
    if (statuses.enabled) {
      nativeOff = await disarm(github, mergeGithub, owner, repo, pr);
      if (!nativeOff) return;
    }
    const ok = await approved(github, owner, repo, pr);
    approvalChecked = true;
    if (!ok) {
      await statuses.set(pr.head.sha, false);
      return await cancel(github, owner, repo, pr, mergeGithub);
    }
    // This gate stays pending until CI is done. In particular fork review events
    // have read-only tokens; the privileged CI-completion event rechecks QA then.
    await statuses.set(pr.head.sha, null);
    if (!statuses.enabled) {
      // Preserve legacy-only early waiting: the required legacy context stays
      // pending until the final status write, with no later release checks.
      const {node} = await github.graphql(`query($id: ID!) {
        node(id: $id) { ... on PullRequest { autoMergeRequest { mergeMethod } } }
      }`, {id: pr.node_id});
      if (node?.autoMergeRequest?.mergeMethod !== 'SQUASH') {
        if (node?.autoMergeRequest) await cancel(github, owner, repo, pr, mergeGithub);
        await enable(mergeGithub, pr);
      }
    }
    const event = await p.labelEvent(github, owner, repo, number);
    const evidence = await ciEvidence(github, owner, repo, pr);
    if (evidence.failed) return await failedCI(github, prGithub, owner, repo, pr, event, evidence.failed, core, mergeGithub, statuses);
    if (!evidence.ready) return;
    if (!await approved(github, owner, repo, pr)) {
      await statuses.set(pr.head.sha, false);
      return await cancel(github, owner, repo, pr, mergeGithub);
    }
    const receipt = await statuses.prepareSuccess(pr.head.sha);
    if (statuses.enabled) {
      // The App write/readback is an additional asynchronous boundary. It must
      // not allow a revoked approval or a new CI attempt to release the old gate.
      if (!await approved(github, owner, repo, pr)) {
        await statuses.set(pr.head.sha, false);
        return await cancel(github, owner, repo, pr, mergeGithub);
      }
      const freshEvent = await p.labelEvent(github, owner, repo, number);
      const fresh = await ciEvidence(github, owner, repo, pr);
      if (fresh.failed) {
        // Cleanup can lose a race to a later label/run and decline to remove it.
        // Revoke the prepared success before delegating that guarded cleanup.
        await statuses.set(pr.head.sha, false);
        return await failedCI(github, prGithub, owner, repo, pr,
          freshEvent, fresh.failed, core, mergeGithub, statuses);
      }
      if (!fresh.ready) return await statuses.set(pr.head.sha, null);
      if (!await approved(github, owner, repo, pr)) {
        await statuses.set(pr.head.sha, false);
        return await cancel(github, owner, repo, pr, mergeGithub);
      }
    }
    await statuses.finishSuccess(pr.head.sha, receipt);
    if (statuses.enabled) {
      // The legacy write/readback is another asynchronous boundary too. Native
      // waiting is still off, even when both contexts have become green.
      if (!await approved(github, owner, repo, pr)) {
        await statuses.set(pr.head.sha, false);
        return await cancel(github, owner, repo, pr, mergeGithub);
      }
      const mode = await nativeReleaseState(github, pr);
      const finalEvent = await p.labelEvent(github, owner, repo, number);
      const finalEvidence = await ciEvidence(github, owner, repo, pr);
      if (finalEvidence.failed) {
        await statuses.set(pr.head.sha, false);
        return await failedCI(github, prGithub, owner, repo, pr,
          finalEvent, finalEvidence.failed, core, mergeGithub, statuses);
      }
      if (!finalEvidence.ready) return await statuses.set(pr.head.sha, null);
      if (!await approved(github, owner, repo, pr)) {
        await statuses.set(pr.head.sha, false);
        return await cancel(github, owner, repo, pr, mergeGithub);
      }
      await statuses.verifySuccess(pr.head.sha, receipt);
      // This policy-enforced, exact-head mutation is the release point. The
      // native state hint selects one API; an error never retries via the other.
      nativeOff = false;
      await release(mergeGithub, pr, mode);
    }
  } catch (error) {
    if (!statuses.enabled && approvalChecked) throw error;
    await invalidate(github, mergeGithub, owner, repo, pr, publisher, core);
    throw error;
  }
}

// PR writes use the installed App. Evidence reads use a read-only App token
// when available, so they do not exhaust the repository's GITHUB_TOKEN quota.
// The legacy required status stays on the Actions token; the App status is
// explicitly opt-in. A schedule recovers PRs whose CI finished while a
// controller run failed (for example on an exhausted API quota).
module.exports = async ({github: actionsGithub, readGithub, context, core, prGithub,
  mergeGithub = prGithub ?? actionsGithub, appQaStatus = false, appId, appInstallationId, repositoryId}) => {
  const github = readGithub ?? actionsGithub;
  const {owner, repo} = context.repo;
  const payload = context.payload;
  // Schedule payloads have no repository object; the job's repository ID does.
  const baseRepositoryId = payload.repository?.id ?? repositoryId;
  if (!prGithub && [payload.repository?.id, repositoryId].includes(1426153186)) {
    throw new Error('The Gesher GitHub App is required for pull request updates');
  }
  let required;
  const appRequired = fresh => fresh ? qaStatuses.appRequiredByRuleset(github, owner, repo) :
    required ??= qaStatuses.appRequiredByRuleset(github, owner, repo);
  const statuses = qaStatuses({github: actionsGithub, readGithub: github, appGithub: prGithub, owner, repo,
    enabled: appQaStatus, appId, installationId: appInstallationId, appRequired, core});
  prGithub ??= github;
  // A bulk/fork event has no single PR concurrency key. It only enqueues work;
  // the dispatched job joins that PR's queue before any approval/status/native
  // mutation. The already-granted App contents:write permission is sufficient.
  const enqueue = async pr => {
    if (pr.state !== 'open' || !Number.isSafeInteger(pr.number) ||
        pr.number <= 0 || !p.SHA.test(pr.head.sha)) return;
    await prGithub.rest.repos.createDispatchEvent({owner, repo, event_type: RECONCILE_EVENT,
      client_payload: {pull_request_number: pr.number, head_sha: pr.head.sha}});
  };
  if (context.eventName === 'repository_dispatch') {
    if (payload.action !== RECONCILE_EVENT || !appQaStatus) return;
    const {pull_request_number: number, head_sha: head} = payload.client_payload ?? {};
    if (!Number.isSafeInteger(number) || number <= 0 || !p.SHA.test(head))
      throw new Error('Invalid queued PR reconciliation');
    return reconcile(github, prGithub, owner, repo, number, core, mergeGithub, statuses, head);
  }
  if (context.eventName === 'workflow_run') {
    const run = payload.workflow_run;
    if (run.event !== 'pull_request' || !p.SHA.test(run.head_sha) || !run.head_repository?.id) return;
    // Same-repository runs name their PRs in the trusted event; fork runs do not.
    const named = (run.pull_requests || []).filter(x => x.base?.ref === 'main' &&
      x.base.repo?.id === baseRepositoryId && Number.isSafeInteger(x.number));
    const prs = named.length ? named :
      await p.list(github.rest.pulls.list, {owner, repo, state: 'open', base: 'main'});
    for (const pr of prs.filter(x => x.head.sha === run.head_sha && x.head.repo?.id === run.head_repository.id)) {
      if (appQaStatus && pr.number !== run.pull_requests?.[0]?.number) {
        const {data: current} = await github.rest.pulls.get({owner, repo, pull_number: pr.number});
        if (current.head.sha === run.head_sha && current.head.repo?.id === run.head_repository.id)
          await enqueue(current);
      } else await reconcile(github, prGithub, owner, repo, pr.number, core, mergeGithub, statuses);
    }
    return;
  }
  if (context.eventName === 'schedule') {
    const gate = appQaStatus ? qaStatuses.APP_STATUS : STATUS;
    // The read token requests statuses access only while the App status is on.
    const statusReader = appQaStatus ? github : actionsGithub;
    const prs = await p.list(github.rest.pulls.list, {owner, repo, state: 'open', base: 'main'});
    for (const pr of prs.filter(x => !x.draft && (p.hasLabel(x) || appQaStatus) && p.SHA.test(x.head.sha))) {
      try {
        if (p.hasLabel(pr)) {
          if (!await stuck(github, statusReader, owner, repo, pr, gate)) continue;
        } else if (!pr.auto_merge) {
          // Recover missed unlabel deliveries too. Without native waiting,
          // only a leftover green App context needs revocation; no CI scan.
          const {latest} = await currentGate(statusReader, owner, repo, pr, gate);
          if (latest?.state !== 'success') continue;
        }
        core.info('#' + pr.number + ': QA gate needs current-evidence reconciliation');
        if (appQaStatus) await enqueue(pr);
        else await reconcile(github, prGithub, owner, repo, pr.number, core, mergeGithub, statuses);
      } catch (error) { core.warning('#' + pr.number + ': ' + error.message); }
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
      return reconcile(github, prGithub, owner, repo, Number(requested), core, mergeGithub, statuses);
    }
    const prs = await p.list(github.rest.pulls.list, {owner, repo, state: 'open', base: 'main'});
    for (const pr of prs) {
      try {
        if (appQaStatus) await enqueue(pr);
        else await reconcile(github, prGithub, owner, repo, pr.number, core, mergeGithub, statuses);
      }
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
    if (statuses.enabled) {
      // A newly recorded native approval can also release an old green gate.
      // Secure the request before label handling writes any review receipt.
      try {
        if (!await disarm(github, mergeGithub, owner, repo, pr)) return;
      } catch (error) {
        await invalidate(github, mergeGithub, owner, repo, pr, statuses, core);
        throw error;
      }
    }
    if (payload.action === 'synchronize') {
      await p.dismissStaleApprovals(github, prGithub, owner, repo, pr);
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
            await p.removeLabel(github, prGithub, owner, repo, pr, event, 'new push requires fresh approval', core);
        }
      }
    }
    if (payload.action === 'labeled') {
      const event = await p.labelEvent(github, owner, repo, number, payload.pull_request.updated_at);
      if (event) {
        const boundary = await p.openingBoundary(github, owner, repo, pr);
        const qa = await p.authorQa(github, owner, repo, pr, event, boundary);
        if (!p.explicitApplication(pr, event, boundary, qa) || event.invalidated) {
          await p.removeLabel(github, prGithub, owner, repo, pr, event, 'fresh label/QA required', core);
        } else if (p.sameTarget(pr, payload.pull_request) && event.unambiguous &&
            event.deliveryMatches && event.actor?.id === payload.sender?.id &&
            await p.authorized(github, owner, repo, event)) {
          const receipt = await p.approval(github, owner, repo, pr, event);
          if (receipt === null) await prGithub.rest.pulls.createReview({owner, repo, pull_number: number,
            commit_id: pr.head.sha, event: 'APPROVE',
            body: '<!-- gesher-automerge-v1 ' + JSON.stringify(p.approvalFor(pr, event)) + ' -->'});
        }
      }
    }
  }
  await reconcile(github, prGithub, owner, repo, number, core, mergeGithub, statuses);
};
module.exports.approved = approved;
module.exports.ciReady = ciReady;
module.exports.STATUS = STATUS;
module.exports.APP_SLUG = p.APP_SLUG;
module.exports.APP_ID = qaStatuses.APP_ID;
module.exports.APP_STATUS = qaStatuses.APP_STATUS;
module.exports.APP_INSTALLATION_ID = qaStatuses.INSTALLATION_ID;
