# ARM64 Windows PR build on a WSL runner

For required framework builds and the current disk-cache policy, see
[self-hosted-ci.md](self-hosted-ci.md). Automatic ARM64 comparisons can remain
off while required jobs use the machine; manual diagnostics still work.

This optional pilot builds current internal PR merge commits on Ubuntu 24.04
inside WSL2. It starts when the existing `arm64-windows` workflow starts, or
manually with that workflow's run ID. It does not replace existing required
checks: keep the hosted baseline while measuring the pilot. This initially
duplicates one build; it does not yet shorten the required CI completion time.
After measuring, routing required builds and updating required-check policy is
a separate rollout. No Android-visible behavior or test target changes.

## Eligibility and access

- The PR must be open, target `main`, and have its head branch in
  `gesher-runtime/gesher`. Its author must currently have write, maintain or
  admin permission. External forks, including forks owned by maintainers,
  are excluded. To test one's own fork, push the reviewed branch into the base
  repository and open an internal PR.
- The hosted authorization job uses only GitHub API metadata and policy from
  `main`. It verifies the current head and both parents of the merge commit,
  then hands the runner an immutable SHA. Conflicting PRs or unavailable merge
  metadata must be retried after the merge ref becomes available.
- A GitHub organization runner group restricts repository access to this
  repository and workflow access to
  `gesher-runtime/gesher/.github/workflows/arm64-windows-self-hosted.yml@refs/heads/main`
  and the reviewed immutable framework policy listed in `self-hosted-ci.md`.
  **Labels alone are not a security boundary.** The management preparation script refuses to
  register a runner if GitHub does not accept and return that exact policy.
  It refuses any other workflow grant and never falls back to the Default group
  or repository-wide registration.
- The build job has only `contents: read`, no persisted checkout credentials,
  separate cache prefixes and distinct artifacts. It is not a trusted artifact
  producer for the ordinary CI pipeline. Trial artifacts must not be substituted
  into production CI or releases.
- The WSL user executes PR code and can access everything that user can access,
  including mounted Windows files and the local network. WSL here is a build
  environment, **not a sandbox for hostile external PRs**. Only internal code
  authored by repository writers is eligible. A VM isolated from personal data
  and the LAN is needed before extending this to untrusted contributions.

## Set up the second Windows computer

First merge this change into `main`. GitHub validates the allowed workflow ref
when creating its runner group and rejects it if the workflow does not exist
there yet. Copying the setup script does not publish the repository changes.

Use two computers: the **management computer** has your authenticated GitHub
CLI; the **runner computer** needs only Windows, WSL2 and Ubuntu-24.04. No `gh`
installation, GitHub account login or personal access token is needed there.

On the management computer, from the repository directory:

```powershell
gh auth status
# Only if not already authenticated with organization admin permissions:
# gh auth login --web --scopes admin:org,repo
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\ci\prepare-wsl-runner.ps1 -OutputDirectory "$HOME\Downloads\gesher-runner-setup"
```

This creates/verifies the server-enforced group restrictions and prepares two
files: `setup-wsl-runner.ps1` and `runner-package.json`. The JSON contains only
public configuration (repository, group, workflow, official runner URL and
SHA-256), with no token or account credentials. The preparation script generates
no registration token. Copy **both files** to the runner computer, for example
into `$HOME\Downloads\gesher-runner-setup`.

An existing classic OAuth token on the management computer may need
`gh auth refresh --scopes admin:org,repo`. Fine-grained credentials need
organization self-hosted-runner write access. A denied group policy/API request
stops preparation; inspect the error rather than weakening the group restriction.

On the runner computer, run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$HOME\Downloads\gesher-runner-setup\setup-wsl-runner.ps1"
```

This installs build dependencies in the existing Ubuntu distribution as Linux
root and verifies the runner archive's checksum before extracting it. Windows
administrator elevation is not required. Only after installation and downloading
finish does it prompt for a temporary registration token.

At that prompt, on the **management computer**, run:

```powershell
gh api --method POST orgs/gesher-runtime/actions/runners/registration-token --jq .token
```

Paste the result into the hidden prompt on the **runner computer**, then press
Enter. This is a short-lived organization registration token (not a PAT); do
not paste it into chat or save it in the setup files. It is passed to WSL over
stdin. After registration, the runner retains its own credentials in its Linux
home directory. It does not retain your GitHub account login.

Wait for `Listening for Jobs`. Keep this PowerShell window open; Ctrl+C stops
the runner. For later restarts, no new account login or registration token is
needed. Run the following on the runner computer:

```powershell
wsl -d Ubuntu-24.04 -- bash -lc 'cd ~/gesher-actions-runner && ./run.sh'
```

There is no Windows startup task or background service in this pilot.
On the management computer, verify that the runner appears online in the
verified group (use the group's ID from the copied JSON):

```powershell
$package = Get-Content "$HOME\Downloads\gesher-runner-setup\runner-package.json" -Raw | ConvertFrom-Json
gh api "orgs/gesher-runtime/actions/runner-groups/$($package.runner_group_id)/runners" --jq '.runners[] | {name, status, busy}'
```

Work and sources are stored under `~/gesher-actions-runner/_work` inside the
Linux filesystem. Check that this distribution's virtual disk resides on the
NVMe C: drive; the Windows partition map alone does not locate a WSL virtual
disk. The job requires 60 GiB free and uses at most eight Ninja processes.
The number of processes is distinct from jobs: the single runner accepts one
job at a time. The usual WSL2 default RAM limit is sufficient for this pilot;
the script does not replace existing `.wslconfig` settings.

If a required download is blocked, use the existing `ssh otzaria` host to fetch
the exact URL and `scp` it back, retaining SHA-256 verification. Do not disable
TLS verification or checksum checks.

## Activate after the change reaches main

The new workflow and its policy script must be on `main` before registration
and activation. Never dispatch the pilot against PR YAML.
In another PowerShell window, authenticated with repository admin access:

```powershell
gh variable set GESHER_SELF_HOSTED_TRIAL --repo gesher-runtime/gesher --body true
```

Open or update an eligible internal PR. Its usual `arm64-windows` run will
trigger `arm64-windows-self-hosted-trial`. The new workflow is visible in Actions;
its check run belongs to `main` because it is a `workflow_run` workflow, not a
replacement PR required check. The authorization summary records the PR number,
merge SHA and hosted baseline URL. The merge SHA is also in the artifact name
and build log. A new base commit can change the merge SHA between baseline and
trial: compare logs and only compare timings for identical source SHAs.

To retry using a baseline run ID (no arbitrary SHA/ref input is accepted):

```powershell
gh run list --repo gesher-runtime/gesher --workflow arm64-windows.yml --limit 10
# Replace 123456789 with the chosen run ID:
gh workflow run arm64-windows-self-hosted.yml --repo gesher-runtime/gesher --ref main -f run-id=123456789
```

Compare build-step time, job time and queue time for identical merges; separate
cold-download runs from warmed-cache runs. The hosted and trial jobs share
`tools/ci/build-arm64-windows.sh`, including all twelve targets and the same
PE/COFF architecture checks. ARM64 execution tests are not part of this build.

Stop scheduling trials with:

```powershell
gh variable set GESHER_SELF_HOSTED_TRIAL --repo gesher-runtime/gesher --body false
```

Cancel already queued trials separately and stop the foreground runner with
Ctrl+C. Remove the runner through the organization's Actions settings when the
pilot is over. Do not delete the WSL distribution; setup uses the existing one.

## Validation

Run the hosted authorization contract tests locally:

```powershell
python -m unittest discover -s tools/ci -p test_self_hosted_trial.py
```

These tests cover API metadata acceptance/rejection, not a real runner, network
download, WSL startup, GitHub group enforcement or an ARM64 compilation. The
first real pilot must verify those behaviors and record its source and limits.
