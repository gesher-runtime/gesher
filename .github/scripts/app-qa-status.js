'use strict';

const {list, APP_SLUG} = require('./merge-approval.js');
const STATUS = 'qa/label-approval';
const APP_STATUS = 'qa/gesher-label-approval';
const APP_ID = 5816428;
const INSTALLATION_ID = 183304638;
const BOT_ID = 358279050;

function trustedStatus(value, owner, repo, sha, context, state) {
  const writer = context === APP_STATUS ? {id: BOT_ID, login: APP_SLUG + '[bot]'} :
    context === STATUS ? {id: 41898282, login: 'github-actions[bot]'} : null;
  return writer !== null && Number.isSafeInteger(value?.id) && value.id > 0 &&
    value.context === context && value.state === state &&
    value.creator?.id === writer.id && value.creator.type === 'Bot' &&
    value.creator.login === writer.login &&
    value.url === `https://api.github.com/repos/${owner}/${repo}/statuses/${sha}`;
}

// During migration the Actions context remains required. Never release it
// before the independently authenticated App status has been read back.
// appRequired() resolves true only when the live branch ruleset requires the App
// context from the App integration. It is consulted only after a legacy write
// fails: then the legacy Actions context is redundant and its write is
// best-effort, so an exhausted GITHUB_TOKEN quota cannot block the gate.
module.exports = ({github, readGithub = github, appGithub, owner, repo, enabled = false, appId,
  installationId, appRequired = async () => false, core}) => {
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
  const legacy = async (sha, ok) => {
    try { return await github.rest.repos.createCommitStatus(args(sha, ok, STATUS)); }
    catch (error) {
      if (!enabled || !await appRequired()) throw error;
      core?.warning(`Legacy ${STATUS} write failed; ${APP_STATUS} is the required gate: ${error.message}`);
      return null;
    }
  };
  const valid = (value, sha, state) => trustedStatus(value, owner, repo, sha, APP_STATUS, state);
  const current = async (sha, receipt) => {
    // Combined status returns the current row per context, avoiding a growing
    // full history scan on every controller event. Still paginate contexts.
    const values = await list(async args => {
      const response = await readGithub.rest.repos.getCombinedStatusForRef(args);
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
    // With native waiting confirmed off, both credentials may be attempted.
    // Otherwise the App must first block an actually required gate, with a
    // confirmed readback. An unrelated legacy write could wake an old green
    // App-only request when cancellation and revocation share a failed token.
    async set(sha, ok, {nativeOff = false} = {}) {
      if (ok !== false && ok !== null) throw new Error('QA success requires two-phase publication');
      if (!enabled) return legacy(sha, ok);
      // In shadow/legacy-only mode even an App write is unrelated to the
      // required gate and can wake an old native request. Establish that this
      // write will block a required gate before touching either credential.
      if (!nativeOff && !await appRequired(true))
        throw new Error('Native waiting is unresolved and the App revocation is not a required gate');
      const errors = [];
      try { await app(sha, ok); } catch (error) { errors.push(error); }
      if (!nativeOff && errors.length) throw errors[0];
      try { await legacy(sha, ok); } catch (error) { errors.push(error); }
      if (errors.length) throw errors[0];
    },
    async prepareSuccess(sha) {
      return enabled ? app(sha, true) : null;
    },
    async finishSuccess(sha, receipt) {
      if (enabled) await current(sha, receipt);
      await legacy(sha, true);
    },
    async verifySuccess(sha, receipt) {
      if (enabled) await current(sha, receipt);
    },
  };
};
// Read the active rules for main; only an App-bound required context counts.
async function appRequiredByRuleset(github, owner, repo) {
  const rules = await list(github.rest.repos.getBranchRules, {owner, repo, branch: 'main'});
  return rules.some(rule => rule?.type === 'required_status_checks' &&
    (rule.parameters?.required_status_checks || []).some(check =>
      check?.context === APP_STATUS && check.integration_id === APP_ID));
}
Object.assign(module.exports, {STATUS, APP_STATUS, APP_ID, INSTALLATION_ID,
  appRequiredByRuleset, trustedStatus});
