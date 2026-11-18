'use strict';

const REPOSITORY = 'gesher-runtime/gesher';
const OWNER_LOGIN = 'palmoni5';
const SHA = /^[0-9a-f]{40}$/;
const LOCAL = {group: 'gesher-build-trial', labels: ['self-hosted', 'linux', 'x64']};

function eligible(context, requested, enabled) {
  const pr = context.payload.pull_request;
  const repo = context.payload.repository;
  return requested === 'true' && enabled === 'true' && context.eventName === 'pull_request' &&
    `${context.repo.owner}/${context.repo.repo}` === REPOSITORY &&
    Number.isSafeInteger(repo?.id) && repo.id > 0 && repo.full_name === REPOSITORY &&
    Number.isSafeInteger(pr?.number) && pr.number > 0 &&
    pr.state === 'open' && pr.user?.login === OWNER_LOGIN &&
    pr.base?.ref === 'main' && pr.base.repo?.id === repo.id && pr.base.repo.full_name === REPOSITORY &&
    pr.head?.repo?.id === repo.id && pr.head.repo.full_name === REPOSITORY &&
    SHA.test(pr.head.sha) && SHA.test(pr.base.sha) && SHA.test(context.sha);
}

async function route({github, context, requested, enabled}) {
  if (!eligible(context, requested, enabled)) return 'ubuntu-24.04';
  const args = {owner: context.repo.owner, repo: context.repo.repo};
  const payload = context.payload.pull_request;
  const {data: pr} = await github.rest.pulls.get({...args, pull_number: payload.number});
  if (pr.state !== 'open' || pr.user.login !== OWNER_LOGIN ||
      pr.user.id !== payload.user.id || pr.head.sha !== payload.head.sha ||
      pr.head.repo?.id !== payload.base.repo.id || pr.head.repo?.full_name !== REPOSITORY ||
      pr.base.repo.id !== payload.base.repo.id || pr.base.repo.full_name !== REPOSITORY ||
      pr.base.ref !== 'main') return 'ubuntu-24.04';
  const {data: permission} = await github.rest.repos.getCollaboratorPermissionLevel(
    {...args, username: OWNER_LOGIN});
  if (!['admin', 'maintain', 'write'].includes(permission.permission)) return 'ubuntu-24.04';
  const {data: commit} = await github.rest.git.getCommit({...args, commit_sha: context.sha});
  if (commit.sha !== context.sha || commit.parents.length !== 2 ||
      commit.parents[1].sha !== payload.head.sha || !SHA.test(commit.parents[0].sha)) {
    throw new Error('PR checkout is not an exact two-parent merge of the requested head');
  }
  const {data: target} = await github.rest.git.getRef({...args, ref: 'heads/main'});
  if (!SHA.test(target.object.sha)) throw new Error('Invalid target tip');
  for (const [base, head] of [[payload.base.sha, commit.parents[0].sha],
                            [commit.parents[0].sha, target.object.sha]]) {
    if (base === head) continue;
    const {data: comparison} = await github.rest.repos.compareCommitsWithBasehead(
      {...args, basehead: `${base}...${head}`});
    if (!['ahead', 'identical'].includes(comparison.status) || comparison.behind_by !== 0) {
      throw new Error('PR merge base is outside the current main history');
    }
  }
  return LOCAL;
}

module.exports = route;
module.exports.eligible = eligible;
