# Dedicated merge-controller runner on the always-on computer

The `automerge` controller handles `labeled`, `unlabeled`, and CI-completion
events. These short API operations use a separate `gesher-automerge` runner
group rather than competing with compilation jobs. The existing build runner
and its group are not granted access to this controller's credentials.

Only `automerge.yml@refs/heads/main` can use the controller pool. PR-ref review
workflows stay hosted. Label and completion events for external PRs can use
the pool because the controller executes main's scripts, not the contributor's
checkout. The new routing expression preserves the existing permission scopes,
App-token issuance/revocation, QA receipts, current-head checks and required CI.

The Linux Hyper-V VM has one virtual CPU, low scheduling weight, dynamic RAM
768 MiB minimum / 1 GiB startup / 2 GiB maximum, and a dynamic 20 GiB disk.
Its unprivileged runner service has a 768 MiB cap, one-CPU quota and low priority.
One controller job executes at a time. It does not remove serialization needed
for events on the same PR or guarantee zero latency when many events arrive.

The guest has no host folder mounts or personal GitHub login. Its account
cannot sudo or read the separate administration home. A private NAT network
preserves existing adapters; guest firewall rules reject new connections to
private/link-local networks. Host administration keys and VM files are accessible
only with the Windows Administrators token or SYSTEM, including through WSL
drive mounts. The host and its administrator remain part of the trust boundary.
Node-based actions inherit the guest's Ubuntu CA bundle for HTTPS verification.

## Install

1. Enable Hyper-V on Windows Pro. If Windows reports `RestartNeeded=True`,
   complete that restart once existing CI work is drained. A running WSL2
   hypervisor does not mean all Hyper-V VM-management components are installed.
2. Publish/review the immutable read-only probe workflow, then on the management
   computer run `tools/ci/prepare-automerge-runner.ps1 -ProbeSHA <published SHA>`.
   It creates/verifies the restricted group and emits three public setup files.
3. Copy that folder to the always-on computer. In elevated PowerShell run:

   ```powershell
   powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\setup-automerge-vm.ps1
   ```

   It uses the existing Ubuntu WSL distribution only to prepare a verified
   Canonical generic cloud image and NoCloud ISO. Azure-specific images are
   not used. The controller then runs in its own Hyper-V VM, independently of WSL.
   It refuses an existing VM/switch, existing NAT or conflicting address range;
   inspect partial installation before proceeding. If the dedicated VM and disk
   already exist and setup stopped while waiting for first-boot SSH, add `-Resume`
   to the install command. This skips image preparation and resumes registration
   with that VM and its existing management key.
4. When the installer reaches registration, obtain a short-lived organization
   runner registration token on the authenticated management computer:

   ```powershell
   gh api --method POST orgs/gesher-runtime/actions/runners/registration-token --jq .token
   ```

   Paste it into the installer's hidden prompt. It travels on SSH standard
   input; it is not bundled, saved to disk or put in a process argument. No
   personal `gh auth login` is needed on the always-on computer or in the guest.
5. Verify `gesher-automerge-local` is online, execute the immutable probe with
   read-only permissions, then merge the normal controller routing PR. Only
   after those checks enable `GESHER_AUTOMERGE_LOCAL=true` or run the management
   helper `tools/ci/set-automerge-runner.ps1 -Target local`.

The probe checks the guest/account/CPU boundary and executes the current main
controller's API fixture tests. It has no write scopes and cannot approve or
merge PRs. A real main controller event must subsequently establish live
execution; fixture success alone is not an end-to-end merge result.

## Return to GitHub

On the authenticated management computer:

```powershell
gh variable set GESHER_AUTOMERGE_LOCAL --repo gesher-runtime/gesher --body false
```

New main controller jobs return to `ubuntu-24.04`. A job already routed to the
local VM retains its assigned route: cancel/rerun an affected controller run if
necessary, retaining the ordinary approval predicates. No VM access or code
change is needed for rollback. There is no automatic hosted failover while
the local machine is asleep/offline. Keep the VM running through active jobs.

Builds were returned to hosted CI separately using `GESHER_SELF_HOSTED_PRS=false`
and `GESHER_SELF_HOSTED_TRIAL=false`; the controller flag does not change those
flags or re-enable compilation in the local controller group.
