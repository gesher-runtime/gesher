'use strict';

// Loaded only from the default branch, never from PR code. A label is an
// opt-in by a user who already has merge-capable repository access. A bot
// receipt binds that label event to a head; trusted PR workflows test that
// head, and the merge API pins it again.
const LABEL = 'automerge';
const RECEIPT = 'gesher-automerge-v1';
const OPENING = 'gesher-automerge-opening-v1';
const QA = 'gesher-automerge-qa-v1';
const PASSING = new Set(['success', 'neutral', 'skipped']);
const BOT = 'github-actions[bot]';
const SHA = /^[a-f0-9]{40}$/i;
const TARGET_CHANGES = new Set(['base_ref_changed', 'automatic_base_change_succeeded']);
const INVALIDATES = new Set(['reopened', 'head_ref_force_pushed', ...TARGET_CHANGES]);

// Incomplete or excessive history requires manual handling, never a merge.
async function list(api, args, field) {
  const all = [];
  for (let page = 1; page <= 10; ++page) {
    const {data} = await api({...args, per_page: 100, page});
    const batch = field ? data[field] : data;
    if (!Array.isArray(batch) || batch.length > 100) throw new Error('invalid API listing');
    all.push(...batch);
    if (batch.length < 100 || (field && data.total_count === all.length)) return all;
  }
  throw new Error('API listing exceeds 1000 items; manual review required');
}

function latestBy(items, keyOf) {
  const latest = new Map();
  for (const item of items) {
    if (!Number.isSafeInteger(item.id) || !Number.isFinite(Date.parse(item.created_at))) {
      throw new Error('missing event/run identity');
    }
    const key = keyOf(item);
    if (key === undefined || key === null) throw new Error('missing listing key');
    const seen = latest.get(key);
    if (!seen || Date.parse(item.created_at) > Date.parse(seen.created_at) ||
        (item.created_at === seen.created_at && item.id > seen.id)) latest.set(key, item);
  }
  return [...latest.values()];
}

function hasLabel(pr) {
  return (pr.labels || []).some((l) => l.name === LABEL);
}

function appliedAfterOpening(pr, event, boundary) {
  const opened = Date.parse(pr.created_at);
  const applied = Date.parse(event.created_at);
  const author = pr.user?.id;
  // GitHub timestamps have second precision. A creation-time label, including
  // an indistinguishable same-second addition, requires a fresh opt-in after
  // initialization. Author opt-in also needs a prior application or QA proof.
  return Number.isSafeInteger(author) && author > 0 &&
    Number.isFinite(opened) && Number.isFinite(applied) &&
    Number.isFinite(boundary) && applied > Math.max(opened, boundary);
}

function needsAuthorQa(pr, event) {
  const previous = Date.parse(event.priorActorApplicationAt);
  return event.actor?.id === pr.user?.id &&
    !(Number.isFinite(previous) && previous >= Date.parse(pr.created_at) &&
      previous < Date.parse(event.created_at));
}

function explicitApplication(pr, event, boundary, qa) {
  return appliedAfterOpening(pr, event, boundary) && (!needsAuthorQa(pr, event) ||
    (qa && qa.actor === event.actor?.id && qa.repository === pr.base.repo.id &&
      qa.number === pr.number && qa.head === pr.head.sha && qa.base === pr.base.ref &&
      Date.parse(qa.submitted) > Math.max(Date.parse(pr.created_at), boundary) &&
      Date.parse(qa.submitted) < Date.parse(event.created_at)));
}

async function authorQa(github, owner, repo, pr, event, boundary) {
  if (!appliedAfterOpening(pr, event, boundary) || !needsAuthorQa(pr, event)) return null;
  const reviews = await list(github.rest.pulls.listReviews, {owner, repo, pull_number: pr.number});
  const candidates = [];
  for (const r of reviews) {
    if (r.state !== 'COMMENTED' || r.user?.id !== event.actor?.id || r.commit_id !== pr.head.sha ||
        !Number.isSafeInteger(r.id) || r.id <= 0 || !Number.isSafeInteger(r.user?.id) || r.user.id <= 0 ||
        typeof r.node_id !== 'string' || !r.node_id || typeof r.submitted_at !== 'string' ||
        !(Date.parse(r.submitted_at) > Math.max(Date.parse(pr.created_at), boundary)) ||
        !(Date.parse(r.submitted_at) < Date.parse(event.created_at)) || typeof r.body !== 'string') continue;
    const markers = r.body.split(/\r?\n/).filter((line) => line.includes(QA));
    if (!markers.length) continue;
    if (markers.length !== 1 || r.body.split(QA).length !== 2 ||
        !markers[0].startsWith('<!-- ' + QA + ' ') || !markers[0].endsWith(' -->')) return null;
    const raw = markers[0].slice(QA.length + 6, -4);
    let value;
    try { value = JSON.parse(raw); } catch { return null; }
    // Compact JSON round-tripping also rejects duplicate keys and ambiguous spellings.
    if (!value || Array.isArray(value) || Object.keys(value).length !== 4 ||
        JSON.stringify(value) !== raw || value.repository !== pr.base.repo.id ||
        value.number !== pr.number || value.head !== pr.head.sha || value.base !== pr.base.ref) return null;
    candidates.push({review: r, proof: {...value, id: r.id, node: r.node_id,
      actor: r.user.id, submitted: r.submitted_at, body: r.body}});
  }
  if (candidates.length !== 1) return null;
  const {review, proof} = candidates[0];
  // REST submitted_at does not change when a review body is edited. Require
  // GitHub's edit metadata so an old comment cannot acquire proof after labeling.
  const {node} = await github.graphql(`query($id: ID!) {
    node(id: $id) { ... on PullRequestReview {
      id body state submittedAt lastEditedAt commit { oid }
    } }
  }`, {id: review.node_id});
  if (node?.id !== review.node_id || node.body !== review.body || node.state !== 'COMMENTED' ||
      node.commit?.oid !== review.commit_id || node.lastEditedAt !== null ||
      Date.parse(node.submittedAt) !== Date.parse(review.submitted_at)) return null;
  return proof;
}

// Read legacy boundaries without creating new initialization comments.
async function openingBoundary(github, owner, repo, pr) {
  const comments = await list(github.rest.issues.listComments,
    {owner, repo, issue_number: pr.number});
  let boundary = null;
  for (const c of comments) {
    if (c.user?.login !== BOT || c.user.type !== 'Bot') continue;
    const markers = (c.body || '').split('\n').filter((s) => s.startsWith('<!-- ' + OPENING + ' '));
    if (markers.length > 1) throw new Error('ambiguous opening receipt');
    if (!markers.length) continue;
    if (!c.created_at || c.created_at !== c.updated_at) throw new Error('edited opening receipt');
    if (!markers[0].endsWith(' -->')) throw new Error('malformed opening receipt');
    const value = JSON.parse(markers[0].slice(OPENING.length + 6, -4));
    if (value.repository !== pr.base.repo.id || value.number !== pr.number ||
        value.created !== pr.created_at) throw new Error('opening receipt identity mismatch');
    const at = Date.parse(value.boundary);
    const recordedAt = Date.parse(c.created_at);
    if (Object.keys(value).length !== 4 || !Number.isFinite(at) ||
        at < Date.parse(pr.created_at) || !Number.isFinite(recordedAt) ||
        recordedAt < Date.parse(pr.created_at)) throw new Error('invalid opening boundary');
    // Concurrent initializers may leave multiple receipts. Never relax the
    // most conservative recorded boundary, regardless of delivery order.
    boundary = Math.max(boundary ?? at, at, recordedAt);
  }
  return boundary ?? Date.parse(pr.created_at);
}

function sameTarget(a, b) {
  return a.number === b.number && a.head.sha === b.head.sha &&
    a.head.repo?.id === b.head.repo?.id && a.head.ref === b.head.ref &&
    a.base.repo?.id === b.base.repo?.id && a.base.ref === b.base.ref;
}

async function labelEvent(github, owner, repo, number, deliveredAt) {
  const events = await list(github.rest.issues.listEvents, {owner, repo, issue_number: number});
  const relevant = events.filter((e) => INVALIDATES.has(e.event) ||
    (['labeled', 'unlabeled'].includes(e.event) && e.label?.name === LABEL));
  const last = latestBy(relevant, () => LABEL)[0];
  if (!last || last.event === 'unlabeled') return null;
  const applied = latestBy(relevant.filter((e) => e.event === 'labeled'), () => LABEL)[0];
  if (!applied) return null;
  // GitHub can stamp the PR update one second after the label event. Bind a
  // delivery only when exactly one label fits that bounded timestamp window.
  const delivered = Date.parse(deliveredAt);
  const candidates = relevant.filter((e) => e.event === 'labeled' &&
    Date.parse(e.created_at) >= delivered - 1000 && Date.parse(e.created_at) <= delivered);
  const previous = latestBy(relevant.filter((e) => e.event === 'labeled' &&
    e.id !== applied.id && e.actor?.id === applied.actor?.id &&
    Date.parse(e.created_at) < Date.parse(applied.created_at)), () => LABEL)[0];
  // A delayed delivery cannot distinguish two relabels by one actor within
  // the webhook timestamp's precision. Do not infer approval in that case.
  return {...applied, invalidated: last.event !== 'labeled',
    priorActorApplicationAt: previous?.created_at,
    deliveryMatches: Number.isFinite(delivered) && candidates.length === 1 && candidates[0].id === applied.id,
    unambiguous: relevant.filter((e) =>
      e.event === 'labeled' && e.created_at === applied.created_at).length === 1};
}

async function authorized(github, owner, repo, event) {
  if (!event?.actor?.login || !Number.isSafeInteger(event.actor.id)) return false;
  try {
    const {data} = await github.rest.repos.getCollaboratorPermissionLevel({
      owner, repo, username: event.actor.login,
    });
    // This API maps maintain to write and triage to read.
    return data.user?.id === event.actor.id &&
      (data.permission === 'admin' || data.permission === 'write');
  } catch (err) {
    if (err.status === 404) return false;
    throw err;
  }
}

function approvalFor(pr, event) {
  return {repository: pr.base.repo.id, number: pr.number, head: pr.head.sha,
    base: pr.base.ref, label: event.id, actor: event.actor.id};
}

async function approval(github, owner, repo, pr, event) {
  const reviews = await list(github.rest.pulls.listReviews,
    {owner, repo, pull_number: pr.number});
  const expected = approvalFor(pr, event);
  const matches = [];
  for (const r of reviews) {
    if (r.user?.login !== BOT || r.user.type !== 'Bot' || !['APPROVED', 'DISMISSED'].includes(r.state)) continue;
    const markers = (r.body || '').split('\n').filter((s) => s.startsWith('<!-- ' + RECEIPT + ' '));
    if (markers.length > 1) throw new Error('ambiguous approval receipt');
    if (!markers.length) continue;
    const marker = markers[0];
    if (!marker.endsWith(' -->')) throw new Error('malformed approval receipt');
    const value = JSON.parse(marker.slice(RECEIPT.length + 6, -4));
    if (value.repository !== expected.repository || value.number !== expected.number ||
        value.label !== expected.label) continue;
    if (r.state === 'DISMISSED') return false;
    if (!r.node_id || !Number.isFinite(Date.parse(r.submitted_at)) ||
        Date.parse(r.submitted_at) < Date.parse(event.created_at)) return false;
    const {node} = await github.graphql(`query($id: ID!) {
      node(id: $id) { ... on PullRequestReview {
        id body state submittedAt lastEditedAt commit { oid }
      } }
    }`, {id: r.node_id});
    if (node?.id !== r.node_id || node.body !== r.body || node.state !== 'APPROVED' ||
        node.commit?.oid !== r.commit_id || node.lastEditedAt !== null ||
        Date.parse(node.submittedAt) !== Date.parse(r.submitted_at)) return false;
    matches.push({...value, head: r.commit_id === value.head ? value.head : null});
  }
  if (!matches.length) return null;
  return matches.every((r) => Object.keys(r).length === Object.keys(expected).length &&
    Object.entries(expected).every(([k, v]) => r[k] === v)) ? expected : false;
}

// Dismiss only this controller's stale approvals, leaving human reviews alone.
async function dismissStaleApprovals(github, owner, repo, pr) {
  const reviews = await list(github.rest.pulls.listReviews,
    {owner, repo, pull_number: pr.number});
  for (const r of reviews) {
    if (r.user?.login === BOT && r.user.type === 'Bot' && r.state === 'APPROVED' &&
        r.commit_id !== pr.head.sha && (r.body || '').includes('<!-- ' + RECEIPT + ' ')) {
      const {data: current} = await github.rest.pulls.get({owner, repo, pull_number: pr.number});
      if (current.state !== 'open' || r.commit_id === current.head.sha) continue;
      await github.rest.pulls.dismissReview({owner, repo, pull_number: pr.number,
        review_id: r.id, message: 'New commits invalidate this automerge approval.'});
    }
  }
}

async function removeLabel(github, owner, repo, pr, event, why, core) {
  // An old job must not remove a newly applied approval.
  const {data: current} = await github.rest.pulls.get({owner, repo, pull_number: pr.number});
  const now = await labelEvent(github, owner, repo, pr.number);
  if (current.state !== 'open' || current.number !== pr.number ||
      !hasLabel(current) || now?.id !== event.id) return;
  try {
    await github.rest.issues.removeLabel({owner, repo, issue_number: pr.number, name: LABEL});
  } catch (err) {
    if (err.status === 404) return;
    throw err;
  }
  core.info('#' + pr.number + ': label removed (' + why + ')');
}

async function targetHead(github, owner, repo, branch) {
  const {data} = await github.rest.git.getRef({owner, repo, ref: 'heads/' + branch});
  if (data.ref !== 'refs/heads/' + branch || data.object?.type !== 'commit' ||
      !SHA.test(data.object.sha || '')) throw new Error('invalid target branch reference');
  return data.object.sha;
}

async function ciVerdict(github, owner, repo, pr, defaultBranch, baseSha) {
  const head = pr.head.sha;
  const runs = await list(github.rest.actions.listWorkflowRunsForRepo,
    {owner, repo, head_sha: head}, 'workflow_runs');
  const required = ['ci.yml'];
  if ([defaultBranch, 'feature/arm64-windows'].includes(pr.base.ref)) required.push('arm64-windows.yml');
  for (const file of required) {
    const {data: workflow} = await github.rest.actions.getWorkflow({owner, repo, workflow_id: file});
    const path = '.github/workflows/' + file;
    if (!Number.isSafeInteger(workflow.id) || workflow.path !== path || workflow.state !== 'active') {
      return 'required workflow is missing or inactive: ' + file;
    }
    const candidates = runs.filter((r) => r.workflow_id === workflow.id && r.path === path &&
      r.event === 'pull_request' && r.head_sha === head &&
      r.head_repository?.id === pr.head.repo.id && r.repository?.id === pr.base.repo.id &&
      r.head_branch === pr.head.ref);
    const selected = latestBy(candidates, () => file)[0];
    if (!selected) return 'no PR run from the required workflow: ' + file;
    const {data: run} = await github.rest.actions.getWorkflowRun({owner, repo, run_id: selected.id});
    if (run.id !== selected.id || run.workflow_id !== workflow.id || run.path !== path ||
        run.event !== 'pull_request' || run.head_sha !== head ||
        run.head_repository?.id !== pr.head.repo.id || run.repository?.id !== pr.base.repo.id ||
        run.head_branch !== pr.head.ref) return 'workflow run identity changed: ' + file;
    if (run.status !== 'completed' || run.conclusion !== 'success') {
      return file + ' has not completed successfully';
    }
    if (!Array.isArray(run.pull_requests)) return 'missing PR association data: ' + file;
    if (run.pull_requests.length && !run.pull_requests.some((p) => p.number === pr.number &&
      p.head?.sha === head && p.head.repo?.id === pr.head.repo.id &&
      p.base?.ref === pr.base.ref && p.base.repo?.id === pr.base.repo.id)) {
      return 'CI is associated with another PR/target: ' + file;
    }
    // Unlike pull_requests (live association data), referenced_workflows
    // records the immutable source GitHub resolved for this particular run.
    {
      const prefix = owner + '/' + repo + '/.github/workflows/';
      const refs = (run.referenced_workflows || []).filter((r) => r.path?.startsWith(prefix));
      const tested = new Set(refs.map((r) => r.sha));
      if (!refs.length || tested.size !== 1 || refs.some((r) =>
        !SHA.test(r.sha || '') || !r.path.endsWith('@' + r.sha) ||
        r.ref !== 'refs/pull/' + pr.number + '/merge')) {
        return 'CI has no unambiguous tested merge identity';
      }
      const mergeSha = refs[0].sha;
      const {data: commit} = await github.rest.git.getCommit({owner, repo, commit_sha: mergeSha});
      if (commit.sha !== mergeSha || commit.parents?.length !== 2 ||
          !SHA.test(commit.parents[0].sha || '') || commit.parents[1].sha !== head) {
        return 'CI has no valid tested base and approved head';
      }
      const testedBase = commit.parents[0].sha;
      if (testedBase !== baseSha) {
        // A reviewed head does not lose its passing CI merely because another
        // PR advanced the target. Prove that the tested base is still in the
        // actual target's history; live PR association data cannot prove this.
        const {data: comparison} = await github.rest.repos.compareCommits({
          owner, repo, base: testedBase, head: baseSha, per_page: 1,
        });
        if (comparison?.status !== 'ahead' ||
            comparison.base_commit?.sha !== testedBase ||
            comparison.merge_base_commit?.sha !== testedBase) {
          return 'CI tested a base outside the current target history';
        }
      }
    }
  }
  // A PR head can equal the default branch SHA. The trusted controller then
  // has its own pending check on this head; it must not wait for itself.
  const {data: controller} = await github.rest.actions.getWorkflow({
    owner, repo, workflow_id: 'automerge.yml',
  });
  if (!Number.isSafeInteger(controller.id) || controller.state !== 'active' ||
      controller.path !== '.github/workflows/automerge.yml') return 'unknown automerge controller';
  const ownRuns = runs.filter((r) => r.workflow_id === controller.id &&
    r.path === controller.path && ['pull_request_target', 'workflow_run', 'schedule'].includes(r.event));
  const ownIds = new Set(ownRuns.map((r) => r.id));
  const ownSuites = new Set(ownRuns.filter((r) => Number.isSafeInteger(r.check_suite_id))
    .map((r) => r.check_suite_id));
  const gatedRuns = runs.filter((r) => !ownIds.has(r.id));
  // A rerun keeps its original created_at. An active older run must not be
  // hidden by a newer success when selecting the latest completed outcomes.
  const active = gatedRuns.find((r) => r.status !== 'completed');
  if (active) return 'workflow still pending: ' + active.name;
  for (const run of latestBy(gatedRuns, (r) => r.workflow_id)) {
    if (!PASSING.has(run.conclusion)) {
      return 'workflow unsuccessful: ' + run.name;
    }
  }
  const checks = await list(github.rest.checks.listForRef,
    {owner, repo, ref: head, filter: 'latest'}, 'check_runs');
  const gatedChecks = checks.filter((c) => !ownSuites.has(c.check_suite?.id));
  if (!gatedChecks.length) return 'no CI check runs for the head';
  for (const check of gatedChecks) {
    if (check.status !== 'completed' || !PASSING.has(check.conclusion)) {
      return 'check still pending or unsuccessful: ' + check.name;
    }
  }
  const statuses = await list(github.rest.repos.listCommitStatusesForRef, {owner, repo, ref: head});
  for (const status of latestBy(statuses, (s) => s.context)) {
    if (status.state !== 'success') return 'commit status is not successful: ' + status.context;
  }
  return null;
}

async function tryMerge({github, core, owner, repo, number, sha, defaultBranch}) {
  const {data: pr} = await github.rest.pulls.get({owner, repo, pull_number: number});
  if (pr.state !== 'open' || pr.draft || !hasLabel(pr)) return;
  if (!SHA.test(pr.head?.sha || '') || !SHA.test(pr.base?.sha || '') ||
      !pr.head.repo?.id || !pr.base.repo?.id || (sha && pr.head.sha !== sha)) return;
  const event = await labelEvent(github, owner, repo, number);
  if (!event || !event.unambiguous || !await authorized(github, owner, repo, event)) return;
  const boundary = await openingBoundary(github, owner, repo, pr);
  const qa = await authorQa(github, owner, repo, pr, event, boundary);
  if (!explicitApplication(pr, event, boundary, qa)) return removeLabel(github, owner, repo, pr, event,
    'apply a later explicit label; an author first label requires an earlier exact-head QA review', core);
  if (event.invalidated) return removeLabel(github, owner, repo, pr, event,
    'the PR changed after approval; apply the label again', core);
  const receipt = await approval(github, owner, repo, pr, event);
  if (receipt === false) return removeLabel(github, owner, repo, pr, event,
    'the approved head or target changed; apply the label again', core);
  if (!receipt) return core.info('#' + number + ': no approval for this head; reapply the label');
  // PR base.sha can retain an older snapshot. Resolve the actual target tip.
  const baseSha = await targetHead(github, owner, repo, pr.base.ref);
  const reason = await ciVerdict(github, owner, repo, pr, defaultBranch, baseSha);
  if (reason) return core.info('#' + number + ': not merging (' + reason + ')');

  // Re-read mutable PR state and the opt-in after all asynchronous CI queries.
  const latest = await labelEvent(github, owner, repo, number);
  const latestBoundary = await openingBoundary(github, owner, repo, pr);
  if (latest?.id !== event.id || latest.invalidated || !latest.unambiguous ||
      !explicitApplication(pr, latest, latestBoundary, qa)) return;
  const finalQa = await authorQa(github, owner, repo, pr, latest, latestBoundary);
  if (JSON.stringify(finalQa) !== JSON.stringify(qa) ||
      !await approval(github, owner, repo, pr, latest) ||
      !await authorized(github, owner, repo, latest)) return;
  const [finalBoundary, finalBase] = await Promise.all([
    openingBoundary(github, owner, repo, pr),
    targetHead(github, owner, repo, pr.base.ref),
  ]);
  const [{data: current}, finalEvent] = await Promise.all([
    github.rest.pulls.get({owner, repo, pull_number: number}),
    labelEvent(github, owner, repo, number),
  ]);
  if (current.state !== 'open' || current.draft || !hasLabel(current) ||
      !sameTarget(pr, current) || current.base.sha !== pr.base.sha || finalBase !== baseSha ||
      finalEvent?.id !== event.id || finalEvent.invalidated || !finalEvent.unambiguous ||
      !explicitApplication(current, finalEvent, finalBoundary, finalQa)) return;
  try {
    const {data: result} = await github.rest.pulls.merge({
      owner, repo, pull_number: number, sha: pr.head.sha, merge_method: 'squash',
    });
    if (result.merged !== true) return removeLabel(github, owner, repo, pr, event,
      'GitHub did not confirm the merge; inspect its merge requirements', core);
  } catch (err) {
    if ([405, 409, 422].includes(err.status)) return removeLabel(github, owner, repo, pr, event,
      'merge failed: ' + err.message, core);
    throw err;
  }
  core.info('#' + number + ': merged ' + pr.head.sha);
  // GITHUB_TOKEN merges do not trigger push workflows. Dispatch trusted main CI.
  if (pr.base.ref === defaultBranch) {
    await github.rest.actions.createWorkflowDispatch({owner, repo, workflow_id: 'ci.yml', ref: defaultBranch});
  }
}

module.exports = async ({github, context, core}) => {
  const {owner, repo} = context.repo;
  const p = context.payload;
  const defaultBranch = p.repository.default_branch;
  const args = {github, core, owner, repo, defaultBranch};
  if (context.eventName === 'pull_request_target') {
    if (p.action === 'labeled' && p.label?.name !== LABEL) return;
    const {data: pr} = await github.rest.pulls.get({owner, repo, pull_number: p.pull_request.number});
    if (pr.state !== 'open') return;
    if (!SHA.test(pr.head?.sha || '') || !SHA.test(pr.base?.sha || '') ||
        !pr.head.repo?.id || !pr.base.repo?.id) return;
    if (p.action === 'synchronize') await dismissStaleApprovals(github, owner, repo, pr);
    if (!hasLabel(pr)) return;
    const event = await labelEvent(github, owner, repo, pr.number,
      p.action === 'labeled' ? p.pull_request.updated_at : undefined);
    if (!event) return;
    // Some opening payloads already include labels with a later updated_at
    // than created_at. Reject that observed initial opt-in, but leave a label
    // applied after the opening snapshot to its own explicit delivery.
    if (p.action === 'opened' && hasLabel(p.pull_request) &&
        Date.parse(event.created_at) <= Date.parse(p.pull_request.updated_at)) {
      return removeLabel(github, owner, repo, pr, event,
        'labels included when opening a PR cannot approve it; apply the label again', core);
    }
    const boundary = await openingBoundary(github, owner, repo, pr);
    const qa = await authorQa(github, owner, repo, pr, event, boundary);
    if (!explicitApplication(pr, event, boundary, qa)) return removeLabel(github, owner, repo, pr, event,
      'apply a later explicit label; an author first label requires an earlier exact-head QA review', core);
    if (event.invalidated) return removeLabel(github, owner, repo, pr, event,
      'the PR changed after approval; apply the label again', core);
    // Opening a PR never grants approval. A delayed opened event must not
    // interfere with an explicit, later label event awaiting its own delivery.
    if (p.action === 'opened') return;
    if (p.action === 'labeled') {
      if (!sameTarget(pr, p.pull_request) || !event.unambiguous ||
          !event.deliveryMatches || event.actor?.id !== p.sender?.id) {
        return core.info('#' + pr.number + ': label delivery is not uniquely bound to this head/event; reapply the label');
      }
      if (!await authorized(github, owner, repo, event)) return removeLabel(
        github, owner, repo, pr, event, 'the label must be applied by a user with write or admin access', core);
      const receipt = await approval(github, owner, repo, pr, event);
      if (receipt === false) return;
      if (!receipt) await github.rest.pulls.createReview({owner, repo, pull_number: pr.number,
        commit_id: pr.head.sha, event: 'APPROVE',
        body: '<!-- ' + RECEIPT + ' ' + JSON.stringify(approvalFor(pr, event)) + ' -->'});
    } else if (p.action === 'synchronize') {
      const receipt = await approval(github, owner, repo, pr, event);
      if (receipt === false || (receipt === null &&
          Date.parse(event.created_at) < Date.parse(p.pull_request.updated_at))) {
        await removeLabel(github, owner, repo, pr, event, 'new commits were pushed; apply the label again', core);
      }
      return;
    }
    return tryMerge({...args, number: pr.number, sha: p.pull_request.head.sha});
  }
  if (context.eventName === 'workflow_run' || context.eventName === 'schedule') {
    const sha = context.eventName === 'workflow_run' ? p.workflow_run.head_sha : null;
    const prs = await list(github.rest.pulls.list, {owner, repo, state: 'open'});
    for (const pr of prs.filter((x) => hasLabel(x) && (!sha || x.head.sha === sha))) {
      try {
        await tryMerge({...args, number: pr.number, sha});
      } catch (err) {
        // Bad history for one PR must not stop the remaining candidates.
        core.warning('#' + pr.number + ': automerge blocked: ' + err.message);
      }
    }
  }
};

module.exports.LABEL = LABEL;
