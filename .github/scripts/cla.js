'use strict';

const fs = require('fs');
const path = require('path');

const CLA_PHRASE = 'I have read the CLA Document and I hereby sign the CLA';
const BOT_MARKER = '<!-- gesher-cla-bot -->';
const SIGNATURES_PATH = '.github/cla/signatures.json';
const STATUS_CONTEXT = 'CLA';

function readSignatures(workspaceRoot) {
  const fullPath = path.join(workspaceRoot, SIGNATURES_PATH);
  if (!fs.existsSync(fullPath)) {
    return { signed: [] };
  }
  const data = JSON.parse(fs.readFileSync(fullPath, 'utf8'));
  if (!Array.isArray(data.signed)) {
    return { signed: [] };
  }
  return data;
}

function isSigned(signatures, login) {
  const target = String(login || '').toLowerCase();
  return signatures.signed.some(
    (entry) => String(entry.login || '').toLowerCase() === target,
  );
}

function isBotLogin(login, userType) {
  if (userType === 'Bot') return true;
  return String(login || '').toLowerCase().endsWith('[bot]');
}

function buildRequestComment(login, repoFullName) {
  const claUrl = `https://github.com/${repoFullName}/blob/main/.github/cla/CLA.md`;
  return [
    BOT_MARKER,
    `שלום @${login}, תודה על התרומה!`,
    '',
    `לפני שנוכל למזג את ה-PR, כל תורם נדרש לחתום פעם אחת על הסכם התורמים ([CLA](${claUrl})).`,
    '',
    'כדי לחתום, אנא פרסם תגובה ב-PR זה עם הנוסח המדויק הבא:',
    '',
    '```text',
    CLA_PHRASE,
    '```',
    '',
    '---',
    `Hi @${login}, thank you for your contribution! Before we can merge this PR, every contributor must sign the [Contributor License Agreement (CLA)](${claUrl}) once.`,
    '',
    'To sign, please post a comment on this PR with the exact text:',
    '',
    '```text',
    CLA_PHRASE,
    '```',
  ].join('\n');
}

async function setClaStatus({ github, owner, repo, sha, state, description }) {
  if (!sha) return;
  await github.rest.repos.createCommitStatus({
    owner,
    repo,
    sha,
    state,
    context: STATUS_CONTEXT,
    description: description.slice(0, 140),
  });
}

async function findExistingBotComment({ github, owner, repo, issueNumber }) {
  const comments = await github.paginate(github.rest.issues.listComments, {
    owner,
    repo,
    issue_number: issueNumber,
    per_page: 100,
  });
  return comments.find((c) => typeof c.body === 'string' && c.body.includes(BOT_MARKER));
}

async function recordSignatureOnDefaultBranch({
  github,
  owner,
  repo,
  defaultBranch,
  login,
  prNumber,
}) {
  const fileRes = await github.rest.repos.getContent({
    owner,
    repo,
    path: SIGNATURES_PATH,
    ref: defaultBranch,
  });
  const currentContent = Buffer.from(fileRes.data.content, 'base64').toString('utf8');
  const data = JSON.parse(currentContent);
  if (!Array.isArray(data.signed)) data.signed = [];
  if (isSigned(data, login)) return false;

  data.signed.push({
    login,
    pr: prNumber,
    signed_at: new Date().toISOString(),
  });
  const updatedContent = `${JSON.stringify(data, null, 2)}\n`;
  await github.rest.repos.createOrUpdateFileContents({
    owner,
    repo,
    path: SIGNATURES_PATH,
    branch: defaultBranch,
    message: `chore(cla): record CLA signature for @${login} (#${prNumber})`,
    content: Buffer.from(updatedContent, 'utf8').toString('base64'),
    sha: fileRes.data.sha,
  });
  return true;
}

async function run({ github, context, core }) {
  const { owner, repo } = context.repo;
  const repoFullName = `${owner}/${repo}`;
  const workspaceRoot = process.env.GITHUB_WORKSPACE || process.cwd();
  const defaultBranch =
    (context.payload.repository && context.payload.repository.default_branch) || 'main';

  if (context.eventName === 'issue_comment') {
    const issue = context.payload.issue;
    const comment = context.payload.comment;
    if (!issue || !issue.pull_request || !comment) {
      core.info('Comment is not on a pull request; skipping.');
      return;
    }

    const commenter = comment.user && comment.user.login;
    const commenterType = comment.user && comment.user.type;
    if (isBotLogin(commenter, commenterType)) {
      return;
    }

    const body = String(comment.body || '').trim();
    if (body !== CLA_PHRASE) {
      return;
    }

    const prRes = await github.rest.pulls.get({
      owner,
      repo,
      pull_number: issue.number,
    });
    const pr = prRes.data;
    const prAuthor = pr.user && pr.user.login;

    await recordSignatureOnDefaultBranch({
      github,
      owner,
      repo,
      defaultBranch,
      login: commenter,
      prNumber: issue.number,
    });

    const signatures = readSignatures(workspaceRoot);
    if (!isSigned(signatures, commenter)) {
      signatures.signed.push({ login: commenter, pr: issue.number });
    }

    if (String(commenter).toLowerCase() === String(prAuthor).toLowerCase()) {
      await setClaStatus({
        github,
        owner,
        repo,
        sha: pr.head.sha,
        state: 'success',
        description: `CLA signed by @${commenter}`,
      });
      const existing = await findExistingBotComment({
        github,
        owner,
        repo,
        issueNumber: issue.number,
      });
      const signedNotice = `${BOT_MARKER}\nתודה @${commenter}, החתימה על ה-CLA נשמרה! / Thank you @${commenter}, your CLA signature has been recorded.`;
      if (existing) {
        await github.rest.issues.updateComment({
          owner,
          repo,
          comment_id: existing.id,
          body: signedNotice,
        });
      } else {
        await github.rest.issues.createComment({
          owner,
          repo,
          issue_number: issue.number,
          body: signedNotice,
        });
      }
    }
    return;
  }

  const pr = context.payload.pull_request;
  if (!pr) return;

  const author = pr.user && pr.user.login;
  const authorType = pr.user && pr.user.type;
  if (isBotLogin(author, authorType)) {
    await setClaStatus({
      github,
      owner,
      repo,
      sha: pr.head.sha,
      state: 'success',
      description: 'Bot PRs do not require a CLA signature',
    });
    return;
  }

  const signatures = readSignatures(workspaceRoot);
  if (isSigned(signatures, author)) {
    await setClaStatus({
      github,
      owner,
      repo,
      sha: pr.head.sha,
      state: 'success',
      description: `CLA signed by @${author}`,
    });
    return;
  }

  await setClaStatus({
    github,
    owner,
    repo,
    sha: pr.head.sha,
    state: 'failure',
    description: `@${author} has not signed the CLA yet`,
  });

  const existing = await findExistingBotComment({
    github,
    owner,
    repo,
    issueNumber: pr.number,
  });
  if (!existing) {
    await github.rest.issues.createComment({
      owner,
      repo,
      issue_number: pr.number,
      body: buildRequestComment(author, repoFullName),
    });
  }
}

module.exports = {
  CLA_PHRASE,
  BOT_MARKER,
  readSignatures,
  isSigned,
  isBotLogin,
  buildRequestComment,
  run,
};
