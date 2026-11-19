'use strict';

const {list, APP_SLUG} = require('./merge-approval.js');
const STATUS = 'qa/label-approval';
const APP_STATUS = 'qa/gesher-label-approval';
const APP_ID = 5816428;
const INSTALLATION_ID = 183304638;
const BOT_ID = 358279050;

// During migration the Actions context remains required. Never release it
// before the independently authenticated App status has been read back.
module.exports = ({github, appGithub, owner, repo, enabled = false, appId, installationId}) => {
  if (typeof enabled !== 'boolean') throw new Error('App QA status requires an explicit boolean flag');
  if (enabled && (!appGithub || appGithub === github || appId !== APP_ID ||
      installationId !== INSTALLATION_ID ||
      typeof appGithub.rest?.repos?.createCommitStatus !== 'function')) {
    throw new Error('Gesher App QA status requires the verified App and installation');
  }
  const args = (sha, ok, context) => ({owner, repo, sha, context,
    state: ok === null ? 'pending' : ok ? 'success' : 'failure', description: ok === null ?
      'Authorized label recorded; waiting for required PR CI' : ok ?
      'Authorized exact-head label, QA and PR CI verified' : 'Fresh authorized automerge label/QA required'});
  const legacy = (sha, ok) => github.rest.repos.createCommitStatus(args(sha, ok, STATUS));
  const valid = (value, sha, state) => {
    return Number.isSafeInteger(value?.id) && value.id > 0 &&
      value.context === APP_STATUS && value.state === state &&
      value.creator?.id === BOT_ID && value.creator.type === 'Bot' &&
      value.creator.login === APP_SLUG + '[bot]' &&
      value.url === `https://api.github.com/repos/${owner}/${repo}/statuses/${sha}`;
  };
  const current = async (sha, receipt) => {
    // Combined status returns the current row per context, avoiding a growing
    // full history scan on every controller event. Still paginate contexts.
    const values = await list(async args => {
      const response = await github.rest.repos.getCombinedStatusForRef(args);
      if (response.data.sha !== sha) throw new Error('Gesher App QA status readback head mismatch');
      return response;
    }, {owner, repo, ref: sha}, 'statuses');
    const matching = values.filter(s => s.context === APP_STATUS);
    if (matching.some(s => !Number.isSafeInteger(s.id) || s.id <= 0))
      throw new Error('Gesher App QA status has an invalid identity');
    const latest = matching.sort((a, b) => b.id - a.id)[0];
    if (!latest || latest.id !== receipt.id || !valid(latest, sha, receipt.state))
      throw new Error('Gesher App QA status is missing, replaced or from a different source');
  };
  const app = async (sha, ok) => {
    const input = args(sha, ok, APP_STATUS);
    const {data} = await appGithub.rest.repos.createCommitStatus(input);
    if (!valid(data, sha, input.state)) throw new Error('Gesher App QA status write identity mismatch');
    await current(sha, data);
    return data;
  };
  return {
    enabled,
    // Attempt both revocations even if one credential fails. A failed shadow
    // write must not leave the existing required gate green.
    async set(sha, ok) {
      if (ok !== false && ok !== null) throw new Error('QA success requires two-phase publication');
      if (!enabled) return legacy(sha, ok);
      const results = await Promise.allSettled([legacy(sha, ok), app(sha, ok)]);
      const rejected = results.find(r => r.status === 'rejected');
      if (rejected) throw rejected.reason;
    },
    async prepareSuccess(sha) {
      return enabled ? app(sha, true) : null;
    },
    async finishSuccess(sha, receipt) {
      if (enabled) await current(sha, receipt);
      await legacy(sha, true);
    },
  };
};
Object.assign(module.exports, {STATUS, APP_STATUS, APP_ID, INSTALLATION_ID});
