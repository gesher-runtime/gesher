#!/usr/bin/env bash
# Run via the VM's management SSH account. Registration token arrives on stdin.
set -euo pipefail
url=$1
digest=$2
[[ $url =~ ^https://github.com/actions/runner/releases/download/v[0-9.]+/actions-runner-linux-x64-[0-9.]+\.tar\.gz$ ]]
[[ $digest =~ ^[0-9a-f]{64}$ ]]
test -f /var/lib/gesher-automerge-ready
systemctl is-active --quiet nftables
cd /home/gesher-runner
test ! -e actions-runner/.runner
install -d -o gesher-runner -g gesher-runner -m 0700 actions-runner
cd actions-runner
curl -fsSL --retry 3 --connect-timeout 20 "$url" -o runner.tar.gz
printf '%s  runner.tar.gz\n' "$digest" | sha256sum --check --strict
sudo -u gesher-runner tar -xzf runner.tar.gz
rm runner.tar.gz
# sudo preserves stdin; the token is not placed in argv, a file or a log.
sudo -u gesher-runner bash -c '
  set -euo pipefail
  read -r registration_token
  registration_token=${registration_token%$'"'"'\r'"'"'}
  ./config.sh --unattended --url https://github.com/gesher-runtime \
    --token "$registration_token" --runnergroup gesher-automerge \
    --name gesher-automerge-local --labels automerge --work _work
  unset registration_token
'
./svc.sh install gesher-runner
service=$(cat .service)
[[ $service =~ ^actions\.runner\.[A-Za-z0-9._-]+\.service$ ]]
mkdir -p "/etc/systemd/system/$service.d"
cat > "/etc/systemd/system/$service.d/resources.conf" <<'UNIT'
[Service]
MemoryMax=768M
CPUQuota=100%
Nice=10
TasksMax=256
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ReadWritePaths=/home/gesher-runner
UNIT
systemctl daemon-reload
./svc.sh start
systemctl show "$service" -p User -p MemoryMax -p CPUQuotaPerSecUSec -p Nice -p NoNewPrivileges
