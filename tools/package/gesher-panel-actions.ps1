# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Background action execution; dot-sourced by the panel in its existing scope.
# Requires $here and GesherUiLanguage; returns the existing PowerShell/Handle/Disposed worker.
# UI ownership, completion and close policy remain in gesher-control-panel.ps1.

# Installs an APK through gesher-app.ps1; on failure throws the panel's message with the
# reason com.gesher.app.Launcher wrote to GESHER_LAUNCH_ERROR_FILE (for example a
# different signing key than the installed app's), not only "failed".
function Install-PanelApk([string] $file, [string] $displayName) {
    $errorFile = $null
    try { $errorFile = [System.IO.Path]::GetTempFileName() } catch { }
    $previous = $env:GESHER_LAUNCH_ERROR_FILE
    if ($errorFile) { $env:GESHER_LAUNCH_ERROR_FILE = $errorFile }
    else { Remove-Item Env:GESHER_LAUNCH_ERROR_FILE -ErrorAction SilentlyContinue }
    $reason = ''
    try {
        & (Join-Path $here 'gesher-app.ps1') install "$file"
        $code = $LASTEXITCODE
    } finally {
        if ($null -eq $previous) { Remove-Item Env:GESHER_LAUNCH_ERROR_FILE -ErrorAction SilentlyContinue }
        else { $env:GESHER_LAUNCH_ERROR_FILE = $previous }
        # Also when gesher-app.ps1 throws: the temporary file never stays behind.
        if ($errorFile) {
            try { $reason = [System.IO.File]::ReadAllText($errorFile, [System.Text.Encoding]::UTF8).Trim() } catch { }
            Remove-Item -LiteralPath $errorFile -Force -ErrorAction SilentlyContinue
        }
    }
    if ($code -ne 0) {
        $message = Get-GesherText 'cp_install_failed' @($displayName)
        if ($reason) { $message += "`n`n" + $reason }
        throw $message
    }
}

# One background owner runs profile mutations. Arguments are data and no WPF
# objects enter its runspace. Existing helper scripts retain their own process
# lifetimes; the panel never aborts a possibly committing install on close.
function New-PanelActionWorker([string] $operation, $request) {
    $powerShell = [PowerShell]::Create()
    try {
        $work = {
            param($here, $operation, $request, $installDefinition, $language)
            $ErrorActionPreference = 'Stop'
            $ProgressPreference = 'SilentlyContinue'
            # This process already has the panel's chosen display language.
            # Reuse it without compiling a native language reader, and without
            # changing GESHER_LOCALES (which also configures Android apps).
            $env:GESHER_PANEL_UI_LANGUAGE = $language
            # Includes nested microG staging helpers. Bound transport waits;
            # committing an installation is deliberately never force-stopped.
            $PSDefaultParameterValues = @{ 'Invoke-WebRequest:TimeoutSec' = 120 }
            . (Join-Path $here 'gesher-ui.ps1')
            $script:GesherUiLanguage = $language
            # Trusted source from this panel, shared with the synchronous helper
            # contracts. No path, label or package is interpolated into source.
            . ([scriptblock]::Create("function Install-PanelApk {`n$installDefinition`n}"))
            function Invoke-PanelApp([string[]] $arguments) {
                $global:LASTEXITCODE = 0
                & (Join-Path $here 'gesher-app.ps1') @arguments 6>$null | Out-Null
                if ($LASTEXITCODE -ne 0) { throw (Get-GesherText 'cp_action_failed' @($LASTEXITCODE)) }
            }
            $failures = [Collections.Generic.List[string]]::new()
            try {
                switch ($operation) {
                    'Device' { Invoke-PanelApp @([string]$request.Command) }
                    'Microg' {
                        Invoke-PanelApp @('shutdown')
                        $global:LASTEXITCODE = 0
                        if ($request.Enable) {
                            & (Join-Path $here 'gesher-app-microg.ps1') -AutoDownload 6>$null | Out-Null
                        } else {
                            & (Join-Path $here 'gesher-app-microg.ps1') -Disable 6>$null | Out-Null
                        }
                        if ($LASTEXITCODE -ne 0) { throw (Get-GesherText 'cp_action_failed' @($LASTEXITCODE)) }
                        Invoke-PanelApp @('start')
                    }
                    'Install' {
                        foreach ($file in $request.Files) {
                            try {
                                Install-PanelApk ([string]$file) ([IO.Path]::GetFileName([string]$file)) 6>$null | Out-Null
                            } catch { $failures.Add($_.Exception.Message) }
                        }
                    }
                    'Store' {
                        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
                        $storeApk = Join-Path ([IO.Path]::GetTempPath()) 'AuroraStore-4.8.4.apk'
                        if (-not (Test-Path -LiteralPath $storeApk)) {
                            $temporary = [IO.Path]::GetTempFileName()
                            try {
                                Invoke-WebRequest -Uri 'https://auroraoss.com/downloads/AuroraStore/Release/AuroraStore-4.8.4.apk' -OutFile $temporary -UseBasicParsing -TimeoutSec 120
                                Move-Item -LiteralPath $temporary -Destination $storeApk -Force
                            } finally {
                                if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
                            }
                        }
                        Install-PanelApk $storeApk 'Aurora Store' 6>$null | Out-Null
                    }
                    'Uninstall' {
                        $global:LASTEXITCODE = 0
                        & (Join-Path $here 'gesher-app.ps1') uninstall ([string]$request.Package) 6>$null | Out-Null
                        if ($LASTEXITCODE -ne 0) { throw (Get-GesherText 'cp_uninstall_failed' @([string]$request.Label)) }
                    }
                    'AppIsolation' {
                        $global:LASTEXITCODE = 0
                        & (Join-Path $here 'gesher-app.ps1') app-isolation ([string]$request.Package) ([string]$request.Mode) 6>$null | Out-Null
                        if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 1223) {
                            throw (Get-GesherText 'cp_action_failed' @($LASTEXITCODE))
                        }
                    }
                    default { throw 'Unknown dashboard operation.' }
                }
            } catch { $failures.Add($_.Exception.Message) }
            [PSCustomObject]@{ Errors = $failures.ToArray() }
        }
        $owner = {
            param($body, $here, $operation, $request, $installDefinition, $language)
            $ErrorActionPreference = 'Stop'
            # ConsoleHost preserves the packaged helpers' native stderr semantics.
            # A bare PowerShell API runspace turns even successful ART diagnostics
            # into terminating errors under ErrorActionPreference=Stop.
            # Declare Unicode Win32 entry points in memory. Runtime CodeDOM compilation
            # uses ANSI-dependent native temporary-path code on some .NET Framework builds.
            # Sequential value types keep pointer-size-dependent layout with the CLR marshaler.
            function Initialize-PanelActionNative {
                if ('Gesher.PanelActionNative' -as [type]) { return }
                $name = [Reflection.AssemblyName]::new('Gesher.PanelActionNative')
                if ($PSVersionTable.PSEdition -eq 'Core') {
                    $assembly = [Reflection.Emit.AssemblyBuilder]::DefineDynamicAssembly($name, [Reflection.Emit.AssemblyBuilderAccess]::Run)
                } else {
                    $assembly = [AppDomain]::CurrentDomain.DefineDynamicAssembly($name, [Reflection.Emit.AssemblyBuilderAccess]::Run)
                }
                $module=$assembly.DefineDynamicModule('Native')
                function Define-Struct([string] $name, $fields) {
                    $builder=$module.DefineType($name,[Reflection.TypeAttributes]'Public,Sealed,SequentialLayout',[ValueType])
                    foreach($name in $fields.Keys) { $null=$builder.DefineField($name,$fields[$name],[Reflection.FieldAttributes]::Public) }
                    $builder.CreateType()
                }
                $security=Define-Struct 'Gesher.PanelActionSecurity' ([ordered]@{Length=[int];Descriptor=[IntPtr];Inherit=[int]})
                $startup=Define-Struct 'Gesher.PanelActionStartup' ([ordered]@{
                    Size=[int];Reserved=[IntPtr];Desktop=[IntPtr];Title=[IntPtr]
                    X=[int];Y=[int];Width=[int];Height=[int];CharsX=[int];CharsY=[int];Fill=[int];Flags=[int]
                    Show=[int16];ReservedSize=[int16];ReservedBytes=[IntPtr];Input=[IntPtr];Output=[IntPtr];Error=[IntPtr]
                })
                $extended=Define-Struct 'Gesher.PanelActionExtendedStartup' ([ordered]@{Startup=$startup;Attributes=[IntPtr]})
                $process=Define-Struct 'Gesher.PanelActionProcessInfo' ([ordered]@{Process=[IntPtr];Thread=[IntPtr];ProcessId=[uint32];ThreadId=[uint32]})
                $builder=$module.DefineType('Gesher.PanelActionNative',[Reflection.TypeAttributes]'Public,Abstract,Sealed')
                $lastError = $builder.DefineField('LastError', [int], [Reflection.FieldAttributes]'Public,Static')
                $threadStatic = [Reflection.Emit.CustomAttributeBuilder]::new([ThreadStaticAttribute].GetConstructor(@()), [object[]]@())
                $lastError.SetCustomAttribute($threadStatic)
                $getLastError = [Runtime.InteropServices.Marshal].GetMethod('GetLastWin32Error')
                $constructor=[Runtime.InteropServices.DllImportAttribute].GetConstructor(@([string]))
                $fields=[Reflection.FieldInfo[]]@([Runtime.InteropServices.DllImportAttribute].GetField('CharSet'),[Runtime.InteropServices.DllImportAttribute].GetField('SetLastError'),[Runtime.InteropServices.DllImportAttribute].GetField('EntryPoint'))
                foreach($signature in @(
                    @{Name='CreateFileW';Return=[IntPtr];Parameters=[Type[]]@([string],[uint32],[uint32],$security.MakeByRefType(),[uint32],[uint32],[IntPtr])},
                    @{Name='InitializeProcThreadAttributeList';Return=[bool];Parameters=[Type[]]@([IntPtr],[int],[uint32],[IntPtr].MakeByRefType())},
                    @{Name='UpdateProcThreadAttribute';Return=[bool];Parameters=[Type[]]@([IntPtr],[uint32],[IntPtr],[IntPtr],[IntPtr],[IntPtr],[IntPtr])},
                    @{Name='DeleteProcThreadAttributeList';Return=[void];Parameters=[Type[]]@([IntPtr])},
                    @{Name='CreateProcessW';Return=[bool];Parameters=[Type[]]@([string],[Text.StringBuilder],[IntPtr],[IntPtr],[bool],[uint32],[IntPtr],[string],$extended.MakeByRefType(),$process.MakeByRefType())},
                    @{Name='WaitForSingleObject';Return=[uint32];Parameters=[Type[]]@([IntPtr],[uint32])},
                    @{Name='GetExitCodeProcess';Return=[bool];Parameters=[Type[]]@([IntPtr],[uint32].MakeByRefType())},
                    @{Name='CloseHandle';Return=[bool];Parameters=[Type[]]@([IntPtr])}
                )) {
                    $values=[object[]]@([Runtime.InteropServices.CharSet]::Unicode,$true,$signature.Name)
                    $attribute=[Reflection.Emit.CustomAttributeBuilder]::new($constructor,[object[]]@('kernel32.dll'),$fields,$values)
                    $native=$builder.DefineMethod(('Native'+$signature.Name),[Reflection.MethodAttributes]'Private,Static,PinvokeImpl',$signature.Return,$signature.Parameters)
                    $native.SetCustomAttribute($attribute)
                    $native.SetImplementationFlags([Reflection.MethodImplAttributes]::PreserveSig)
                    # PowerShell's binder can invoke other APIs before a later
                    # GetLastWin32Error call. Preserve this call's error in managed
                    # IL before crossing that boundary, isolated per native thread.
                    $method=$builder.DefineMethod($signature.Name,[Reflection.MethodAttributes]'Public,Static',$signature.Return,$signature.Parameters)
                    $il=$method.GetILGenerator()
                    for($argument=0;$argument -lt $signature.Parameters.Count;$argument++) {
                        $il.Emit([Reflection.Emit.OpCodes]::Ldarg,[int16]$argument)
                    }
                    $il.Emit([Reflection.Emit.OpCodes]::Call,$native)
                    $result=$null
                    if($signature.Return -ne [void]) {
                        $result=$il.DeclareLocal($signature.Return)
                        $il.Emit([Reflection.Emit.OpCodes]::Stloc,$result)
                    }
                    $il.Emit([Reflection.Emit.OpCodes]::Call,$getLastError)
                    $il.Emit([Reflection.Emit.OpCodes]::Stsfld,$lastError)
                    if($result) { $il.Emit([Reflection.Emit.OpCodes]::Ldloc,$result) }
                    $il.Emit([Reflection.Emit.OpCodes]::Ret)
                }
                $null=$builder.CreateType()
            }

            function Invoke-PanelActionNative([string] $executable,[string] $entry,[string] $here) {
                Initialize-PanelActionNative
                $security=[Gesher.PanelActionSecurity]::new()
                $security.Length=[Runtime.InteropServices.Marshal]::SizeOf([type][Gesher.PanelActionSecurity])
                $security.Inherit=1
                $inputHandle=[IntPtr]::Zero; $outputHandle=[IntPtr]::Zero; $attributes=[IntPtr]::Zero; $handles=[IntPtr]::Zero
                $initialized=$false
                $process=[Gesher.PanelActionProcessInfo]::new()
                function Throw-NativeFailure {
                    $errorCode=[Gesher.PanelActionNative]::LastError
                    throw [ComponentModel.Win32Exception]::new($errorCode)
                }
                function Valid([IntPtr] $handle) { $handle -ne [IntPtr]::Zero -and $handle -ne [IntPtr]::new(-1) }
                try {
                    $inputHandle=[Gesher.PanelActionNative]::CreateFileW('NUL',2147483648,3,[ref]$security,3,0,[IntPtr]::Zero)
                    if(-not (Valid $inputHandle)) { Throw-NativeFailure }
                    $outputHandle=[Gesher.PanelActionNative]::CreateFileW('NUL',1073741824,3,[ref]$security,3,0,[IntPtr]::Zero)
                    if(-not (Valid $outputHandle)) { Throw-NativeFailure }
                    $size=[IntPtr]::Zero
                    $null=[Gesher.PanelActionNative]::InitializeProcThreadAttributeList([IntPtr]::Zero,1,0,[ref]$size)
                    if($size -eq [IntPtr]::Zero) { Throw-NativeFailure }
                    $attributes=[Runtime.InteropServices.Marshal]::AllocHGlobal($size)
                    if(-not [Gesher.PanelActionNative]::InitializeProcThreadAttributeList($attributes,1,0,[ref]$size)) { Throw-NativeFailure }
                    $initialized=$true
                    $handles=[Runtime.InteropServices.Marshal]::AllocHGlobal(2*[IntPtr]::Size)
                    [Runtime.InteropServices.Marshal]::WriteIntPtr($handles,0,$inputHandle)
                    [Runtime.InteropServices.Marshal]::WriteIntPtr($handles,[IntPtr]::Size,$outputHandle)
                    if(-not [Gesher.PanelActionNative]::UpdateProcThreadAttribute($attributes,0,[IntPtr]::new(0x20002),$handles,[IntPtr]::new(2*[IntPtr]::Size),[IntPtr]::Zero,[IntPtr]::Zero)) { Throw-NativeFailure }
                    $startup=[Gesher.PanelActionStartup]::new()
                    $startup.Size=[Runtime.InteropServices.Marshal]::SizeOf([type][Gesher.PanelActionExtendedStartup])
                    $startup.Flags=0x100; $startup.Input=$inputHandle; $startup.Output=$outputHandle; $startup.Error=$outputHandle
                    $extended=[Gesher.PanelActionExtendedStartup]::new()
                    $extended.Startup=$startup; $extended.Attributes=$attributes
                    $command=[Text.StringBuilder]::new('"'+$executable+'" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "'+$entry+'"')
                    if(-not [Gesher.PanelActionNative]::CreateProcessW($executable,$command,[IntPtr]::Zero,[IntPtr]::Zero,$true,0x08080000,[IntPtr]::Zero,$here,[ref]$extended,[ref]$process)) { Throw-NativeFailure }
                    $null=[Gesher.PanelActionNative]::CloseHandle($process.Thread); $process.Thread=[IntPtr]::Zero
                    if([Gesher.PanelActionNative]::WaitForSingleObject($process.Process,[uint32]::MaxValue) -ne 0) { Throw-NativeFailure }
                    $code=[uint32]0
                    if(-not [Gesher.PanelActionNative]::GetExitCodeProcess($process.Process,[ref]$code)) { Throw-NativeFailure }
                    return [int][BitConverter]::ToInt32([BitConverter]::GetBytes($code),0)
                } finally {
                    if(Valid $process.Thread) { $null=[Gesher.PanelActionNative]::CloseHandle($process.Thread) }
                    if(Valid $process.Process) { $null=[Gesher.PanelActionNative]::CloseHandle($process.Process) }
                    if($initialized) { [Gesher.PanelActionNative]::DeleteProcThreadAttributeList($attributes) }
                    if($attributes -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::FreeHGlobal($attributes) }
                    if($handles -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::FreeHGlobal($handles) }
                    if(Valid $inputHandle) { $null=[Gesher.PanelActionNative]::CloseHandle($inputHandle) }
                    if(Valid $outputHandle) { $null=[Gesher.PanelActionNative]::CloseHandle($outputHandle) }
                }
            }
            $directory = Join-Path ([IO.Path]::GetTempPath()) ('gesher-panel-action-' + [guid]::NewGuid().ToString('N'))
            $null = [IO.Directory]::CreateDirectory($directory)
            $configuration = Join-Path $directory 'request.json'
            $resultPath = Join-Path $directory 'result.json'
            $entry = Join-Path $directory 'action.ps1'
            try {
                $data = @{ Body = $body; Here = $here; Operation = $operation; Request = $request
                    InstallDefinition = $installDefinition; Language = $language }
                [IO.File]::WriteAllText($configuration, ($data | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($true))
                $hostScript = @'
$ErrorActionPreference = 'Stop'
$resultPath = Join-Path $PSScriptRoot 'result.json'
try {
    $config = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'request.json')) | ConvertFrom-Json
    $result = & ([scriptblock]::Create($config.Body)) $config.Here $config.Operation $config.Request $config.InstallDefinition $config.Language
    [IO.File]::WriteAllText($resultPath, ($result | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($true))
} catch {
    [IO.File]::WriteAllText($resultPath, ([PSCustomObject]@{ Errors = @($_.Exception.Message) } | ConvertTo-Json), [Text.UTF8Encoding]::new($true))
    exit 1
}
'@
                [IO.File]::WriteAllText($entry, $hostScript, [Text.UTF8Encoding]::new($true))
                $executable = Join-Path ([Environment]::GetFolderPath('System')) 'WindowsPowerShell/v1.0/powershell.exe'
                $exitCode = (Invoke-PanelActionNative $executable $entry $here)
                if (-not [IO.File]::Exists($resultPath)) { throw "Dashboard operation exited without a result (exit code $exitCode)." }
                [IO.File]::ReadAllText($resultPath) | ConvertFrom-Json
            } finally {
                # Only this owner's three known files and empty directory are
                # removed, after its native process has exited. No recursive delete.
                foreach ($path in $entry, $configuration, $resultPath) { [IO.File]::Delete($path) }
                [IO.Directory]::Delete($directory, $false)
            }
        }
        $null = $powerShell.AddScript($owner.ToString()).AddArgument($work.ToString()).AddArgument($here).AddArgument($operation).
            AddArgument($request).AddArgument((Get-Command Install-PanelApk).Definition).AddArgument($script:GesherUiLanguage)
        $handle = $powerShell.BeginInvoke()
        return [PSCustomObject]@{ PowerShell = $powerShell; Handle = $handle; Disposed = $false }
    } catch {
        $powerShell.Dispose()
        throw
    }
}
