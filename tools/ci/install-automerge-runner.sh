#!/usr/bin/env bash
# Run via the VM's management SSH account. Registration token arrives on stdin.
set -euo pipefail
url=$1
digest=$2
slot=${3:-1}
[[ $url =~ ^https://github.com/actions/runner/releases/download/v[0-9.]+/actions-runner-linux-x64-[0-9.]+\.tar\.gz$ ]]
[[ $digest =~ ^[0-9a-f]{64}$ ]]
[[ $slot =~ ^[123]$ ]]
test -f /var/lib/gesher-automerge-ready
systemctl is-active --quiet nftables
exec 9>/run/lock/gesher-automerge-install.lock
flock -n 9
account=gesher-runner
runner=gesher-automerge-local
if [[ $slot != 1 ]]; then
  test "$(nproc)" -ge 3
  account+=-$slot
  runner+=-$slot
  if ! getent passwd "$account" >/dev/null; then
    useradd --create-home --user-group --shell /usr/sbin/nologin "$account"
  fi
fi
home=/home/$account
test ! -L "$home"
test "$(stat -c %U "$home")" = "$account"
chmod 0700 "$home"
cd "$home"
test ! -L actions-runner
install -d -o "$account" -g "$account" -m 0700 actions-runner
cd actions-runner
if [[ ! -f .runner ]]; then
  install -d -m 0700 /var/cache/gesher-automerge
  archive=/var/cache/gesher-automerge/$digest.tar.gz
  if [[ ! -f $archive ]] || ! printf '%s  %s\n' "$digest" "$archive" | sha256sum --check --status; then
    download=$(mktemp /var/cache/gesher-automerge/download.XXXXXX)
    trap 'rm -f -- "$download"' EXIT
    curl -fL --retry 3 --connect-timeout 20 --max-time 900 "$url" -o "$download"
    printf '%s  %s\n' "$digest" "$download" | sha256sum --check --strict
    mv -f -- "$download" "$archive"
    trap - EXIT
  fi
  sudo -Hu "$account" tar -xzf - < "$archive"
  # The registration token is read on stdin and passed in the runner input environment.
  sudo -Hu "$account" bash -c '
    set -euo pipefail
    IFS= read -r ACTIONS_RUNNER_INPUT_TOKEN
    ACTIONS_RUNNER_INPUT_TOKEN=${ACTIONS_RUNNER_INPUT_TOKEN%$'"'"'\r'"'"'}
    export ACTIONS_RUNNER_INPUT_TOKEN
    exec ./config.sh --unattended --url https://github.com/gesher-runtime \
      --runnergroup gesher-automerge --name "$1" --labels automerge --work _work
  ' bash "$runner"
fi
sudo -Hu "$account" python3 - "$runner" <<'PY'
import json, sys
with open('.runner', encoding='utf-8-sig') as stream:
    settings = json.load(stream)
sys.exit(settings.get('agentName', settings.get('AgentName')) != sys.argv[1])
PY
if [[ ! -f .service ]]; then
  ./svc.sh install "$account"
fi
service=$(cat .service)
[[ $service == "actions.runner.gesher-runtime.$runner.service" ]]
mkdir -p "/etc/systemd/system/$service.d"
cat > "/etc/systemd/system/$service.d/resources.conf" <<'UNIT'
[Service]
Environment=NODE_EXTRA_CA_CERTS=/etc/ssl/certs/ca-certificates.crt
MemoryMax=768M
CPUQuota=100%
Nice=10
TasksMax=256
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
UNIT
printf 'ReadWritePaths=%s\n' "$home" >> "/etc/systemd/system/$service.d/resources.conf"
systemctl daemon-reload
systemctl start "$service"
systemctl is-active --quiet "$service"
systemctl show "$service" -p User -p MemoryMax -p CPUQuotaPerSecUSec -p Nice -p NoNewPrivileges
