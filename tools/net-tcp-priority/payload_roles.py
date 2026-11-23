# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 The Gesher Authors
"""The existing complete TCP/guarded-UDP member roles, shared by typed suppliers."""
ROLES = {
    'gesher-exec.exe': 'host-bionic-loader',
    'net_tcp_priority_probe.exe': 'host-posix-contract',
    'gesher_arm64.dll': 'matching-arm64-translator',
    'tcp-priority-x86_64': 'ndk-x86_64-contract',
    'tcp-priority-arm64-v8a': 'ndk-arm64-contract',
    'net_udp_guarded_probe.exe': 'host-posix-guarded-udp',
    'net_udp_guarded_fault_probe.exe': 'host-posix-guarded-udp-faults',
    'udp-guarded-x86_64': 'ndk-x86_64-guarded-udp',
    'udp-guarded-arm64-v8a': 'ndk-arm64-guarded-udp',
    'udp-guarded-fault-x86_64': 'ndk-x86_64-guarded-udp-faults',
    'udp-guarded-fault-arm64-v8a': 'ndk-arm64-guarded-udp-faults',
    'udp-guarded-linux.txt': 'actual-linux-guarded-udp-reference',
    'udp-guarded-fault-linux.txt': 'actual-linux-guarded-udp-fault-reference',
}
