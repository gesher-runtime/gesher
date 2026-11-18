'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const route = require('./route.cjs');

function fixture() {
  const repo = {id: 7, full_name: 'gesher-runtime/gesher'};
  const pr = {number: 42, state: 'open', user: {id: 8, login: 'palmoni5'},
    base: {ref: 'main', repo, sha: '1'.repeat(40)},
    head: {repo, sha: '2'.repeat(40)}};
  return {repo: {owner: 'gesher-runtime', repo: 'gesher'}, eventName: 'pull_request',
    sha: '3'.repeat(40), payload: {repository: repo, pull_request: pr}};
}

function api(context) {
  return {rest: {
    pulls: {get: async () => ({data: structuredClone(context.payload.pull_request)})},
    repos: {getCollaboratorPermissionLevel: async () => ({data: {permission: 'write'}}),
      compareCommitsWithBasehead: async () => ({data: {status: 'ahead', behind_by: 0}})},
    git: {getCommit: async () => ({data: {sha: context.sha,
      parents: [{sha: '1'.repeat(40)}, {sha: '2'.repeat(40)}]}}),
      getRef: async () => ({data: {object: {sha: '1'.repeat(40)}}})}
  }};
}

test('eligible owner PR gets the restricted group', async () => {
  const context = fixture();
  assert.equal((await route({context, github: api(context), requested: 'true', enabled: 'true'})).group,
    'gesher-build-trial');
});

test('forks, other authors, queue groups and disabled rollout stay hosted without API calls', async () => {
  const variants = [
    c => { c.payload.pull_request.head.repo = {id: 99, full_name: 'palmoni5/fork'}; },
    c => { c.payload.pull_request.user.login = 'someone'; },
    c => { c.eventName = 'merge_group'; },
    c => { c.eventName = 'workflow_dispatch'; },
    c => { c.payload.pull_request.base.ref = 'other'; },
    c => { c.payload.pull_request.head.repo = null; }
  ];
  for (const change of variants) {
    const context = fixture(); change(context);
    assert.equal(await route({context, github: {}, requested: 'true', enabled: 'true'}), 'ubuntu-24.04');
  }
  assert.equal(await route({context: fixture(), github: {}, requested: 'false', enabled: 'true'}), 'ubuntu-24.04');
  assert.equal(await route({context: fixture(), github: {}, requested: 'true', enabled: 'false'}), 'ubuntu-24.04');
});

test('author without current write access falls back to hosted', async () => {
  const context = fixture(), github = api(context);
  github.rest.repos.getCollaboratorPermissionLevel = async () => ({data: {permission: 'read'}});
  assert.equal(await route({context, github, requested: 'true', enabled: 'true'}), 'ubuntu-24.04');
});

test('stale head falls back to hosted', async () => {
  const context = fixture(), github = api(context);
  github.rest.pulls.get = async () => ({data: {...context.payload.pull_request,
    head: {...context.payload.pull_request.head, sha: '4'.repeat(40)}}});
  assert.equal(await route({context, github, requested: 'true', enabled: 'true'}), 'ubuntu-24.04');
});

test('wrong merge parent fails closed', async () => {
  const context = fixture(), github = api(context);
  github.rest.git.getCommit = async () => ({data: {sha: context.sha, parents: []}});
  await assert.rejects(route({context, github, requested: 'true', enabled: 'true'}), /two-parent/);
});

test('advanced target is accepted only with proven ancestry', async () => {
  const context = fixture(), github = api(context);
  github.rest.git.getRef = async () => ({data: {object: {sha: '5'.repeat(40)}}});
  assert.equal((await route({context, github, requested: 'true', enabled: 'true'})).group, 'gesher-build-trial');
  github.rest.repos.compareCommitsWithBasehead = async () => ({data: {status: 'diverged', behind_by: 1}});
  await assert.rejects(route({context, github, requested: 'true', enabled: 'true'}), /outside/);
});
