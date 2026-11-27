'use strict';

const {APP_ID, INSTALLATION_ID} = require('./app-qa-status.js');
const {SHA} = require('./merge-approval.js');

// Advisory: this reports the controller's actual outcome, never grants approval.
module.exports = ({github, enabled, appId, installationId, owner, repo, context, core}) => {
  if (!enabled) return async () => {};
  if (!github || appId !== APP_ID || installationId !== INSTALLATION_ID)
    throw new Error('Automerge check requires the verified Gesher App installation');
  return async (pr, outcome) => {
    try {
      if (!Number.isSafeInteger(pr.number) || pr.number <= 0 || !SHA.test(pr.head?.sha) ||
          pr.base?.ref !== 'main' || !['success', 'neutral', 'failure'].includes(outcome))
        throw new Error('Invalid controller outcome');
      const {data: current} = await github.rest.pulls.get({owner, repo, pull_number: pr.number});
      if (current.head.sha !== pr.head.sha || current.base.ref !== pr.base.ref || current.auto_merge) return;
      const title = outcome === 'success' ? 'Reviewed head merged' : outcome === 'failure' ?
        'Automerge reconciliation failed' : 'Waiting for merge requirements';
      const summary = outcome === 'success' ?
        'The controller completed the protected squash merge for this exact head.' : outcome === 'failure' ?
        'The controller encountered an error. See the workflow logs and required QA/CI results.' :
        'No merge was performed. Review the required QA/CI results, approval and branch rules.';
      await github.rest.checks.create({owner, repo, name: 'automerge', head_sha: pr.head.sha,
        status: 'completed', conclusion: outcome, output: {title, summary},
        ...(Number.isSafeInteger(context.runId) && context.runId > 0 ? {
          details_url: `${context.serverUrl || 'https://github.com'}/${owner}/${repo}/actions/runs/${context.runId}`,
        } : {})});
    } catch (error) {
      core.warning('Automerge check could not be published: ' + error.message);
    }
  };
};
