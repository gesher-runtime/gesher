'use strict';

// Loaded only from the default branch, never from PR code. A label is an
// opt-in by a user who already has merge-capable repository access. A bot
// receipt binds that label event to a head; the native queue tests the combined
// source. The native waiting mutation pins the approved head again.
const LABEL = 'automerge';
const RECEIPT = 'gesher-automerge-v1';
const OPENING = 'gesher-automerge-opening-v1';
const QA = 'gesher-automerge-qa-v1';
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

module.exports = {list, hasLabel, labelEvent, openingBoundary, authorQa,
  explicitApplication, authorized, approval, approvalFor, sameTarget,
  dismissStaleApprovals, removeLabel, SHA};
