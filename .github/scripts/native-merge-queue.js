'use strict';

// Only loaded from main. GitHub owns CI waiting and the actual merge.
const p = require('./merge-approval.js');
const STATUS = 'qa/label-approval';

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
    state: ok ? 'success' : 'failure', description: ok ?
      'Authorized exact-head label and QA receipt verified' : 'Fresh authorized automerge label/QA required'});
}

async function cancel(github, pr) {
  const {node} = await github.graphql(`query($id: ID!) {
    node(id: $id) { ... on PullRequest { autoMergeRequest { enabledAt } mergeQueueEntry { id } } }
  }`, {id: pr.node_id});
  if (node?.autoMergeRequest) await github.graphql(`mutation($id: ID!) {
    disablePullRequestAutoMerge(input: {pullRequestId: $id}) { clientMutationId }
  }`, {id: pr.node_id});
  if (node?.mergeQueueEntry) await github.graphql(`mutation($id: ID!) {
    dequeuePullRequest(input: {pullRequestId: $id}) { clientMutationId }
  }`, {id: pr.node_id});
}

async function reconcile(github, owner, repo, number) {
  const {data: pr} = await github.rest.pulls.get({owner, repo, pull_number: number});
  if (pr.state !== 'open') return;
  let ok = false;
  try { ok = await approved(github, owner, repo, pr); }
  catch (error) {
    await status(github, owner, repo, pr.head.sha, false);
    await cancel(github, pr);
    throw error;
  }
  await status(github, owner, repo, pr.head.sha, ok);
  if (!ok) return cancel(github, pr);
  // Fail closed if configuration is missing; never fall back to direct merging.
  const {repository} = await github.graphql(`query($owner: String!, $repo: String!) {
    repository(owner: $owner, name: $repo) { mergeQueue(branch: "main") { id } }
  }`, {owner, repo});
  if (!repository?.mergeQueue) throw new Error('Native merge queue is not enabled for main');
  const {node} = await github.graphql(`query($id: ID!) {
    node(id: $id) { ... on PullRequest { autoMergeRequest { enabledAt } mergeQueueEntry { id } } }
  }`, {id: pr.node_id});
  if (node?.autoMergeRequest || node?.mergeQueueEntry) return;
  await github.graphql(`mutation($id: ID!, $head: GitObjectID!) {
    enablePullRequestAutoMerge(input: {pullRequestId: $id, expectedHeadOid: $head,
      mergeMethod: SQUASH}) { clientMutationId }
  }`, {id: pr.node_id, head: pr.head.sha});
}

async function groupApproval(github, owner, repo, group) {
  if (group.base_ref !== 'refs/heads/main' || !p.SHA.test(group.head_sha))
    throw new Error('Unexpected merge group identity');
  const {repository} = await github.graphql(`query($owner: String!, $repo: String!) {
    repository(owner: $owner, name: $repo) {
      mergeQueue(branch: "main") { entries(first: 100) { pageInfo { hasNextPage }
        nodes { position headCommit { oid } pullRequest { number } }
      } }
    }
  }`, {owner, repo});
  const entries = repository?.mergeQueue?.entries;
  if (!entries || entries.pageInfo.hasNextPage) throw new Error('Incomplete merge queue inventory');
  const matching = entries.nodes.filter(e => e.headCommit?.oid === group.head_sha);
  if (matching.length !== 1) throw new Error('Merge group is not uniquely bound to a queue entry');
  const included = entries.nodes.filter(e => e.position <= matching[0].position);
  if (!included.length) throw new Error('Empty merge group');
  for (const entry of included) {
    const {data: pr} = await github.rest.pulls.get({owner, repo, pull_number: entry.pullRequest.number});
    if (!await approved(github, owner, repo, pr)) throw new Error('Queue member lacks exact-head approval: #' + pr.number);
  }
}

module.exports = async ({github, context, core}) => {
  const {owner, repo} = context.repo;
  const payload = context.payload;
  if (context.eventName === 'merge_group') {
    try {
      await groupApproval(github, owner, repo, payload.merge_group);
      await status(github, owner, repo, payload.merge_group.head_sha, true);
    } catch (error) {
      await status(github, owner, repo, payload.merge_group.head_sha, false);
      throw error;
    }
    return;
  }
  if (context.eventName === 'workflow_dispatch') {
    const prs = await p.list(github.rest.pulls.list, {owner, repo, state: 'open', base: 'main'});
    for (const pr of prs) {
      try { await reconcile(github, owner, repo, pr.number); }
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
    if (payload.action === 'synchronize') await p.dismissStaleApprovals(github, owner, repo, pr);
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
  await reconcile(github, owner, repo, number);
};
module.exports.approved = approved;
module.exports.groupApproval = groupApproval;
module.exports.STATUS = STATUS;
