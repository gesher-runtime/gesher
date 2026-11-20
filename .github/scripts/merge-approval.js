'use strict';

// Loaded only from the default branch, never from PR code. A label is an
// opt-in by a user who already has merge-capable repository access. A bot
// receipt binds that label event to a head; the native queue tests the combined
// source. The native waiting mutation pins the approved head again.
const LABEL = 'automerge';
const RECEIPT = 'gesher-automerge-v1';
const OPENING = 'gesher-automerge-opening-v1';
const BOT = 'github-actions[bot]';
const APP_SLUG = 'gesher-runtime-automation';
const RECEIPT_BOTS = new Map([[BOT, 41898282], [`${APP_SLUG}[bot]`, 358279050]]);
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
  // initialization. Any later application by a write/admin collaborator,
  // including the PR author, is an explicit approval of the current head.
  return Number.isSafeInteger(author) && author > 0 &&
    Number.isFinite(opened) && Number.isFinite(applied) &&
    Number.isFinite(boundary) && applied > Math.max(opened, boundary);
}

function explicitApplication(pr, event, boundary) {
  return appliedAfterOpening(pr, event, boundary);
}

// Read legacy boundaries without creating new initialization comments.
async function openingBoundary(github, owner, repo, pr) {
  const comments = await list(github.rest.issues.listComments,
    {owner, repo, issue_number: pr.number});
  let boundary = null;
  for (const c of comments) {
    if (!isReceiptBot(c.user)) continue;
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
  // A delayed delivery cannot distinguish two relabels by one actor within
  // the webhook timestamp's precision. Do not infer approval in that case.
  return {...applied, invalidated: last.event !== 'labeled',
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

function isReceiptBot(user) {
  return user?.type === 'Bot' && Number.isSafeInteger(user.id) &&
    RECEIPT_BOTS.get(user.login) === user.id;
}

async function approval(github, owner, repo, pr, event) {
  const reviews = await list(github.rest.pulls.listReviews,
    {owner, repo, pull_number: pr.number});
  const expected = approvalFor(pr, event);
  const matches = [];
  for (const r of reviews) {
    if (!isReceiptBot(r.user) || !['APPROVED', 'DISMISSED'].includes(r.state)) continue;
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
async function dismissStaleApprovals(github, prGithub, owner, repo, pr) {
  const reviews = await list(github.rest.pulls.listReviews,
    {owner, repo, pull_number: pr.number});
  for (const r of reviews) {
    if (isReceiptBot(r.user) && r.state === 'APPROVED' &&
        r.commit_id !== pr.head.sha && (r.body || '').includes('<!-- ' + RECEIPT + ' ')) {
      const {data: current} = await github.rest.pulls.get({owner, repo, pull_number: pr.number});
      if (current.state !== 'open' || r.commit_id === current.head.sha) continue;
      await prGithub.rest.pulls.dismissReview({owner, repo, pull_number: pr.number,
        review_id: r.id, message: 'New commits invalidate this automerge approval.'});
    }
  }
}

async function removeLabel(github, prGithub, owner, repo, pr, event, why, core) {
  // An old job must not remove a newly applied approval.
  const {data: current} = await github.rest.pulls.get({owner, repo, pull_number: pr.number});
  const now = await labelEvent(github, owner, repo, pr.number);
  if (current.state !== 'open' || current.number !== pr.number ||
      !sameTarget(pr, current) || !hasLabel(current) || now?.id !== event.id) return false;
  try {
    await prGithub.rest.issues.removeLabel({owner, repo, issue_number: pr.number, name: LABEL});
  } catch (err) {
    if (err.status === 404) return false;
    throw err;
  }
  core.info('#' + pr.number + ': label removed (' + why + ')');
  return true;
}

module.exports = {list, hasLabel, labelEvent, openingBoundary,
  explicitApplication, authorized, approval, approvalFor, sameTarget,
  dismissStaleApprovals, removeLabel, isReceiptBot, SHA, APP_SLUG};
