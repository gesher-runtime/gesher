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

function quotaError(status, message, responseStatus = status) {
  return Object.assign(new Error('PRIVATE request details must not be logged'), {
    status, response: {status: responseStatus, data: {message}}
  });
}

test('confirmed quota at every authorization read uses hosted without retry or later reads', async () => {
  const steps = ['pull', 'permission', 'commit', 'ref', 'compare', 'compare'];
  const messages = ['API rate limit exceeded for installation.',
    'You have exceeded a secondary rate limit. Please wait.'];
  for (const status of [403, 429]) {
    for (const message of messages) {
      for (let failure = 0; failure < steps.length; failure++) {
        const context = fixture(), github = api(context), calls = [], warnings = [];
        github.rest.git.getCommit = async () => ({data: {sha: context.sha,
          parents: [{sha: '4'.repeat(40)}, {sha: context.payload.pull_request.head.sha}]}});
        github.rest.git.getRef = async () => ({data: {object: {sha: '5'.repeat(40)}}});
        for (const [group, method, name] of [
          ['pulls', 'get', 'pull'], ['repos', 'getCollaboratorPermissionLevel', 'permission'],
          ['git', 'getCommit', 'commit'], ['git', 'getRef', 'ref'],
          ['repos', 'compareCommitsWithBasehead', 'compare']]) {
          const original = github.rest[group][method];
          github.rest[group][method] = async (...args) => {
            calls.push(name);
            if (calls.length === failure + 1) throw quotaError(status, message);
            return original(...args);
          };
        }
        const result = await route({context, github, requested: 'true', enabled: 'true',
          warn: message => warnings.push(message)});
        assert.equal(result, 'ubuntu-24.04');
        assert.deepEqual(calls, steps.slice(0, failure + 1));
        assert.deepEqual(warnings,
          [`BUILD_RUNNER_ROUTE_RATE_LIMIT status=${status} runner=ubuntu-24.04`]);
      }
    }
  }
});

test('permission denials, malformed quota evidence and other errors remain failures', async () => {
  const primary = 'API rate limit exceeded for installation.';
  const errors = [
    ...[401, 403, 404, 429, 500].map(status => quotaError(status, 'Resource not accessible by integration')),
    quotaError(401, primary), quotaError(404, primary), quotaError(500, primary),
    quotaError(403, primary, 429), quotaError(429, primary, 403),
    quotaError(403, primary + 'x'.repeat(16384)), quotaError(403, null),
    Object.assign(new Error(primary), {status: 403}),
    Object.assign(new Error(primary), {status: 403, response: {status: 403, data: primary}}),
    new Error('PR checkout is not an exact two-parent merge of the requested head')
  ];
  for (const error of errors) {
    const context = fixture(), github = api(context), warnings = [];
    let reads = 0;
    github.rest.pulls.get = async () => { reads++; throw error; };
    await assert.rejects(route({context, github, requested: 'true', enabled: 'true',
      warn: message => warnings.push(message)}), actual => actual === error);
    assert.equal(reads, 1);
    assert.deepEqual(warnings, []);
  }
});
