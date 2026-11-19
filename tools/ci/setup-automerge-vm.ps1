# Run as Administrator on the always-on runner computer, after enabling Hyper-V.
[CmdletBinding()]
param([string]$Distribution='Ubuntu-24.04', [string]$PackagePath='', [switch]$Resume)
$ErrorActionPreference='Stop'
if (!$PackagePath) { $PackagePath=Join-Path $PSScriptRoot 'automerge-package.json' }
$package=Get-Content -LiteralPath $PackagePath -Raw | ConvertFrom-Json
if ($package.repository -cne 'gesher-runtime/gesher' -or $package.runner_group -cne 'gesher-automerge' -or $package.runner_group_id -le 0 -or $package.sha256 -cnotmatch '^[0-9a-f]{64}$' -or $package.download_url -cnotmatch '^https://github.com/actions/runner/releases/download/v[0-9.]+/actions-runner-linux-x64-[0-9.]+\.tar\.gz$') { throw 'Invalid public setup package.' }
if ((Get-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V-All).State -ne 'Enabled') { throw 'Enable Hyper-V and finish its requested restart first.' }
if ((Get-Service vmms).Status -ne 'Running') { throw 'Hyper-V management service must be running.' }
$vmName='Gesher-Automerge'
$switchName='Gesher-Automerge-Private'
$natName='Gesher-Automerge-NAT'
$root=Join-Path $env:ProgramData 'Gesher/AutomergeRunner'
$key=Join-Path $root 'management-key'
$stage=Join-Path $env:TEMP 'gesher-automerge-bootstrap'
$utf8=New-Object System.Text.UTF8Encoding($false)

function Register-GuestRunner {
    $sshArgs=@('-o','BatchMode=yes','-o','ConnectTimeout=5','-o','ServerAliveInterval=5','-o','ServerAliveCountMax=2','-o','StrictHostKeyChecking=accept-new','-o',('UserKnownHostsFile='+(Join-Path $root 'known_hosts')),'-i',$key,'gesher-control@172.29.241.2')
    Write-Host 'Waiting for Ubuntu to complete its first boot and package installation...'
    $ready=$false
    for ($attempt=0;$attempt -lt 120;$attempt++) {
        $sshExit= & {
            # Windows PowerShell 5.1 turns even redirected native stderr into
            # a terminating error under Stop. During startup, retry by exit code.
            $ErrorActionPreference='Continue'
            $null = & ssh.exe @sshArgs 'test -f /var/lib/gesher-automerge-ready' 2>$null
            $LASTEXITCODE
        }
        if ($sshExit -eq 0) { $ready=$true; break }
        Start-Sleep -Seconds 5
    }
    if (!$ready) { throw 'The VM did not become ready. Open its Hyper-V console and check Ubuntu boot and cloud-init.' }

    $guestScript=Join-Path $PSScriptRoot 'install-automerge-runner.sh'
    $guestCopy=Join-Path $stage 'install-automerge-runner.sh'
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    [IO.File]::WriteAllText($guestCopy,(Get-Content -LiteralPath $guestScript -Raw).Replace("`r",''),$utf8)
    & scp.exe -o ('UserKnownHostsFile='+(Join-Path $root 'known_hosts')) -i $key $guestCopy 'gesher-control@172.29.241.2:/tmp/install-automerge-runner.sh'
    if ($LASTEXITCODE -ne 0) { throw 'Could not copy guest installer.' }
    Write-Host 'VM is ready. Registration requires only a temporary token, not a GitHub login on this computer.'
    $secureToken=Read-Host 'Paste the short-lived organization runner registration token (hidden)' -AsSecureString
    $pointer=[IntPtr]::Zero
    try {
        $pointer=[Runtime.InteropServices.Marshal]::SecureStringToBSTR($secureToken)
        $token=[Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer)
        if ([string]::IsNullOrWhiteSpace($token) -or $token -match '[\r\n]') { throw 'A single registration token is required.' }
        $token | & ssh.exe @sshArgs ('sudo bash /tmp/install-automerge-runner.sh '+$package.download_url+' '+$package.sha256)
        if ($LASTEXITCODE -ne 0) { throw 'Guest runner registration or service start failed.' }
    } finally {
        $token=$null
        if ($pointer -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer) }
        $secureToken.Dispose()
    }
    Write-Host 'Dedicated runner installed. Leave local routing disabled until the read-only probe passes.'
}

if ($Resume) {
    $vm=Get-VM -Name $vmName
    $adapters=@(Get-VMNetworkAdapter -VM $vm)
    $disks=@(Get-VMHardDiskDrive -VM $vm)
    if ($adapters.Count -ne 1 -or $adapters[0].SwitchName -ne $switchName -or $disks.Count -ne 1 -or $disks[0].Path -ne (Join-Path $root 'automerge.vhdx') -or !(Test-Path -LiteralPath $key -PathType Leaf)) {
        throw 'The existing VM does not match the dedicated controller setup.'
    }
    if ($vm.State -eq 'Off') { Start-VM -VM $vm }
    elseif ($vm.State -ne 'Running') { throw "Inspect the existing VM state before resuming: $($vm.State)" }
    Register-GuestRunner
    return
}

if (Get-VM -Name $vmName -ErrorAction SilentlyContinue) { throw 'VM exists; inspect it instead of rerunning installation.' }
if (Get-VMSwitch -Name $switchName -ErrorAction SilentlyContinue) { throw 'Dedicated switch exists; inspect partial installation.' }
if (Get-NetNat) { throw 'Review existing host NAT before adding the dedicated private network.' }
if (Get-NetIPAddress -AddressFamily IPv4 | Where-Object IPAddress -like '172.29.241.*') { throw 'Dedicated address range already in use.' }
if ((Get-PSDrive C).Free -lt 30GB) { throw 'At least 30 GiB free space is required.' }
New-Item -ItemType Directory -Path $root -Force | Out-Null
# The build WSL account must not be able to read the VM management key through
# its Windows drive mount. Require the elevated Administrators token, not the
# interactive user's SID, and keep SYSTEM access for Hyper-V.
$acl=New-Object System.Security.AccessControl.DirectorySecurity
$admins=New-Object System.Security.Principal.SecurityIdentifier('S-1-5-32-544')
$system=New-Object System.Security.Principal.SecurityIdentifier('S-1-5-18')
$acl.SetAccessRuleProtection($true,$false)
$acl.SetOwner($admins)
foreach ($identity in @($admins,$system)) {
    $rule=New-Object System.Security.AccessControl.FileSystemAccessRule($identity,'FullControl','ContainerInherit,ObjectInherit','None','Allow')
    $acl.AddAccessRule($rule)
}
Set-Acl -LiteralPath $root -AclObject $acl
if (!(Test-Path -LiteralPath $key)) {
    $keygen=Get-Command ssh-keygen.exe -ErrorAction Stop
    $keyInfo=New-Object System.Diagnostics.ProcessStartInfo
    $keyInfo.FileName=$keygen.Source
    $keyInfo.Arguments='-q -t ed25519 -C gesher-automerge-admin -N "" -f "'+$key+'"'
    $keyInfo.UseShellExecute=$false
    $keyInfo.CreateNoWindow=$true
    $keyProcess=[Diagnostics.Process]::Start($keyInfo)
    $keyProcess.WaitForExit()
    if ($keyProcess.ExitCode -ne 0) { throw 'SSH key generation failed.' }
}
$publicKey=(Get-Content -LiteralPath ($key+'.pub') -Raw).Trim()
if ($publicKey -notmatch '^ssh-ed25519 [A-Za-z0-9+/=]+(?: .*)?$') { throw 'Unexpected SSH public key.' }
New-Item -ItemType Directory -Path $stage -Force | Out-Null
$userData=@'
#cloud-config
hostname: gesher-automerge
manage_etc_hosts: true
ssh_pwauth: false
disable_root: true
users:
  - name: gesher-control
    shell: /bin/bash
    lock_passwd: true
    sudo: ['ALL=(ALL) NOPASSWD:ALL']
    ssh_authorized_keys:
      - __KEY__
  - name: gesher-runner
    shell: /usr/sbin/nologin
    lock_passwd: true
package_update: true
packages: [git, curl, ca-certificates, libicu74, libssl3t64, libkrb5-3, zlib1g, nftables]
write_files:
  - path: /etc/nftables.conf
    permissions: '0600'
    content: |
      flush ruleset
      table inet runner_isolation {
        chain output {
          type filter hook output priority 0; policy accept;
          ct state established,related accept
          ip daddr { 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, 169.254.0.0/16 } reject
          ip6 daddr { fc00::/7, fe80::/10 } reject
        }
      }
runcmd:
  - [systemctl, enable, --now, nftables]
  - [chmod, '0700', /home/gesher-control]
  - [touch, /var/lib/gesher-automerge-ready]
'@
$network=@'
version: 2
ethernets:
  runner:
    match: {name: 'e*'}
    set-name: eth0
    dhcp4: false
    addresses: [172.29.241.2/29]
    routes: [{to: default, via: 172.29.241.1}]
    nameservers: {addresses: [1.1.1.1, 8.8.8.8]}
'@
[IO.File]::WriteAllText((Join-Path $stage 'user-data'),$userData.Replace('__KEY__',$publicKey).Replace("`r",''),$utf8)
[IO.File]::WriteAllText((Join-Path $stage 'network-config'),$network.Replace("`r",''),$utf8)
[IO.File]::WriteAllText((Join-Path $stage 'meta-data'),"instance-id: gesher-automerge-v1`nlocal-hostname: gesher-automerge`n",$utf8)
# WSL may pass commands through the Linux shell. Forward slashes prevent that
# shell from consuming Windows backslashes in PowerShell 5.1 native arguments.
$stageWindows=$stage.Replace('\','/')
$stageLinux=((& wsl.exe -d $Distribution -- wslpath -u -a $stageWindows) -join '').Trim()
if ($LASTEXITCODE -ne 0 -or $stageLinux -notmatch '^/mnt/[a-z]/' -or $stageLinux -match "['`r`n]") { throw 'Cannot resolve a safe WSL bootstrap directory.' }
& wsl.exe -d $Distribution -u root -- apt-get update
if ($LASTEXITCODE -ne 0) { throw 'WSL dependency update failed.' }
& wsl.exe -d $Distribution -u root -- env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends qemu-utils cloud-image-utils genisoimage ca-certificates curl
if ($LASTEXITCODE -ne 0) { throw 'WSL image utility installation failed.' }
$imageScript=@'
set -euo pipefail
stage=$1
work=/root/gesher-automerge-bootstrap
mkdir -p "$work"
cd "$work"
curl -fsSL --retry 3 --connect-timeout 20 https://cloud-images.ubuntu.com/noble/current/SHA256SUMS -o SHA256SUMS
curl -fsSL --retry 3 --connect-timeout 20 https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img -o ubuntu.img
expected=$(awk '$2=="*noble-server-cloudimg-amd64.img" || $2=="noble-server-cloudimg-amd64.img" {print $1}' SHA256SUMS)
[[ $expected =~ ^[0-9a-f]{64}$ ]]
printf '%s  ubuntu.img\n' "$expected" | sha256sum -c -
qemu-img resize ubuntu.img 20G
qemu-img convert -f qcow2 -O vhdx -o subformat=dynamic ubuntu.img "$stage/automerge.vhdx"
cloud-localds --network-config "$stage/network-config" "$stage/seed.iso" "$stage/user-data" "$stage/meta-data"
'@
$encoded=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($imageScript.Replace("`r",'')))
& wsl.exe -d $Distribution -u root -- bash -c "printf '%s' '$encoded' | base64 -d | bash -s -- '$stageLinux'"
if ($LASTEXITCODE -ne 0) { throw 'VM image preparation failed. Keep the routing flag off; inspect download logs.' }
$disk=Join-Path $root 'automerge.vhdx'
$seed=Join-Path $root 'seed.iso'
Copy-Item -LiteralPath (Join-Path $stage 'automerge.vhdx') -Destination $disk
Copy-Item -LiteralPath (Join-Path $stage 'seed.iso') -Destination $seed
New-VMSwitch -Name $switchName -SwitchType Internal | Out-Null
New-NetIPAddress -InterfaceAlias "vEthernet ($switchName)" -IPAddress 172.29.241.1 -PrefixLength 29 | Out-Null
New-NetNat -Name $natName -InternalIPInterfaceAddressPrefix '172.29.241.0/29' | Out-Null
New-VM -Name $vmName -Generation 2 -MemoryStartupBytes 1GB -VHDPath $disk -SwitchName $switchName -Path $root | Out-Null
Set-VMProcessor -VMName $vmName -Count 1 -Maximum 100 -RelativeWeight 50
Set-VMMemory -VMName $vmName -DynamicMemoryEnabled $true -MinimumBytes 768MB -StartupBytes 1GB -MaximumBytes 2GB -Priority 20 -Buffer 10
Set-VMFirmware -VMName $vmName -EnableSecureBoot On -SecureBootTemplate MicrosoftUEFICertificateAuthority
Add-VMDvdDrive -VMName $vmName -Path $seed | Out-Null
Set-VM -Name $vmName -AutomaticStartAction Start -AutomaticStartDelay 30 -AutomaticStopAction ShutDown -CheckpointType Disabled
Start-VM -Name $vmName
Register-GuestRunner
