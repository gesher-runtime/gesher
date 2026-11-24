# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Focused host checks execute the production PowerShell paths. The packaged
# Windows/ART lifecycle, copied profile and Setup checks are separate in
# PortableRuntimeChecks.ps1 and require the actual installer and release ZIP.
param(
    [string] $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$package = Join-Path $RepoRoot 'tools/package'
& (Join-Path $PSScriptRoot 'PortableProfileChecks.ps1') -PackageDirectory $package
& (Join-Path $PSScriptRoot 'PortableAddonChecks.ps1') -PackageDirectory $package
