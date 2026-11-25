# Private-desktop capture of the cache ICO location, NOT Explorer evidence.
# This cache preview does not exercise the shell's later Extract badge composition.
param([Parameter(Mandatory)][string]$Receipt,
      [Parameter(Mandatory)][string]$OutputPng, [switch]$PrivateChild)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
$Receipt=[IO.Path]::GetFullPath($Receipt)
$OutputPng=[IO.Path]::GetFullPath($OutputPng)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class PrivateIconDesktop {
 [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] public struct SI {
  public int cb; public string reserved,desktop,title; public int x,y,cx,cy,xc,yc,fill,flags;
  public short show,cbReserved; public IntPtr reserved2,input,output,error;
 }
 [StructLayout(LayoutKind.Sequential)] public struct PI { public IntPtr process,thread; public int pid,tid; }
 [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr CreateDesktop(string n,IntPtr d,IntPtr dm,int f,uint access,IntPtr sa);
 [DllImport("user32.dll")] static extern bool CloseDesktop(IntPtr h);
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool CreateProcess(string app,System.Text.StringBuilder cmd,IntPtr pa,IntPtr ta,bool inherit,uint flags,IntPtr env,string cwd,ref SI si,out PI pi);
 [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h,uint ms);
 [DllImport("kernel32.dll")] static extern bool GetExitCodeProcess(IntPtr p,out uint code);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
 [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr h,uint code);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd,IntPtr hdc,uint flags);
 [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)] public static extern IntPtr LoadImage(IntPtr instance,string path,uint type,int x,int y,uint flags);
 [DllImport("user32.dll")] public static extern bool DestroyIcon(IntPtr icon);
 public static void Run(string app,string command,string cwd) {
  string name="GesherIconProbe-"+Guid.NewGuid().ToString("N");
  IntPtr desktop=CreateDesktop(name,IntPtr.Zero,IntPtr.Zero,0,0x01ff,IntPtr.Zero);
  if(desktop==IntPtr.Zero) throw new System.ComponentModel.Win32Exception();
  PI pi=new PI();
  try {
   SI si=new SI(); si.cb=Marshal.SizeOf(si); si.desktop=name;
   if(!CreateProcess(app,new System.Text.StringBuilder(command),IntPtr.Zero,IntPtr.Zero,false,0x08000000,IntPtr.Zero,cwd,ref si,out pi)) throw new System.ComponentModel.Win32Exception();
   if(WaitForSingleObject(pi.process,30000)!=0) { TerminateProcess(pi.process,1); throw new Exception("Private preview timed out"); }
   uint code=1; if(!GetExitCodeProcess(pi.process,out code)||code!=0) throw new Exception("Private preview failed: "+code);
  } finally {if(pi.thread!=IntPtr.Zero)CloseHandle(pi.thread);if(pi.process!=IntPtr.Zero)CloseHandle(pi.process);CloseDesktop(desktop);}
 }
}
'@
if(-not $PrivateChild){
 $exe=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
 $child="try { & '"+$PSCommandPath.Replace("'","''")+"' -Receipt '"+$Receipt.Replace("'","''")+"' -OutputPng '"+$OutputPng.Replace("'","''")+"' -PrivateChild } catch { `$_ | Out-String | Set-Content -LiteralPath '"+($OutputPng+'.error.txt').Replace("'","''")+"'; exit 1 }"
 $encoded=[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($child))
 $cmd='"'+$exe+'" -NoProfile -ExecutionPolicy Bypass -EncodedCommand '+$encoded
 [PrivateIconDesktop]::Run($exe,$cmd,$PSScriptRoot)
 if(-not(Test-Path -LiteralPath $OutputPng)){throw 'No screenshot produced'}
 Write-Output "Private harness screenshot: $OutputPng (NOT Explorer)"
 return
}
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
$r=Get-Content -LiteralPath $Receipt -Raw|ConvertFrom-Json
if(-not $r.passed -or $r.installedApks -ne 0){throw 'Passing isolated receipt required'}
if((Get-FileHash -LiteralPath $r.icon).Hash -ne $r.iconSha256){throw 'Icon changed'}
$work=Split-Path $Receipt
$env:LOCALAPPDATA=Join-Path $work 'local'
$probe=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'tracks/55-shell.ps1') -Raw
$begin=$probe.IndexOf('using System;',$probe.IndexOf('GesherShellExtProbe.Handler'))
$end=$probe.IndexOf("'@",$begin)
$code=$probe.Substring($begin,$end-$begin).Replace('where T : Delegate','where T : class').Replace('return Marshal.GetDelegateForFunctionPointer<T>(Marshal.ReadIntPtr(vtable, slot * IntPtr.Size));','return (T)(object)Marshal.GetDelegateForFunctionPointer(Marshal.ReadIntPtr(vtable, slot * IntPtr.Size), typeof(T));')
Add-Type -TypeDefinition $code
$flags=[uint32]0
$actual=[GesherShellExtProbe.Handler]::IconOf((Join-Path $work 'extension/gesher_shellext.dll'),$r.apk,[ref]$flags)
if($actual -ne $r.icon){throw 'COM result differs from receipt'}
$installed=@(Get-ChildItem (Join-Path $work 'local/Gesher/data/app') -Filter '*.apk' -Recurse -ErrorAction SilentlyContinue)
if($installed.Count){throw 'Test profile contains installed APKs'}
$form=New-Object Windows.Forms.Form
$form.Text='Gesher APK icon - private COM harness (NOT Explorer)'
$form.ClientSize=New-Object Drawing.Size(900,510)
$form.BackColor=[Drawing.Color]::White
$form.Font=New-Object Drawing.Font('Segoe UI',11)
function LabelAt($text,$x,$y,$width,$height){
 $label=New-Object Windows.Forms.Label;$label.Text=$text;$label.SetBounds($x,$y,$width,$height);$form.Controls.Add($label)
}
LabelAt 'Real Gett cached ICO returned by GetIconLocation (before shell badge)' 24 20 850 32
LabelAt 'Isolated Windows display harness - NOT an Explorer window' 24 55 850 28
$icons=@()
foreach($entry in @(@($actual,85,'COM result: Gett'),@((Join-Path $r.runtime 'gesher.ico'),520,'Gesher fallback (reference only)'))){
 $handle=[PrivateIconDesktop]::LoadImage([IntPtr]::Zero,$entry[0],1,256,256,0x10)
 if($handle -eq [IntPtr]::Zero){throw 'Windows icon decode failed'}
 $ico=[Drawing.Icon]::FromHandle($handle)
 $picture=New-Object Windows.Forms.PictureBox;$picture.SetBounds($entry[1],100,256,256)
 try{$picture.Image=$ico.ToBitmap()}finally{$ico.Dispose();[void][PrivateIconDesktop]::DestroyIcon($handle)}
 $picture.SizeMode='Zoom';$form.Controls.Add($picture)
 LabelAt $entry[2] $entry[1] 365 350 30
}
LabelAt 'No APK installed in test profile. Real APK / real native COM result; no mock icon.' 24 412 850 28
LabelAt ('DLL SHA256: '+$r.dllSha256.Substring(0,24)+'... (full inputs in validation receipt)') 24 446 850 28
LabelAt 'Explorer refresh and background-worker behavior remain unverified.' 24 475 850 28
$captureError=$null
$timer=New-Object Windows.Forms.Timer;$timer.Interval=1200
$timer.Add_Tick({
 $timer.Stop()
 try{
  $bitmap=New-Object Drawing.Bitmap($form.Width,$form.Height)
  $graphics=[Drawing.Graphics]::FromImage($bitmap);$hdc=$graphics.GetHdc()
  try{if(-not[PrivateIconDesktop]::PrintWindow($form.Handle,$hdc,2)){throw 'PrintWindow failed'}}
  finally{$graphics.ReleaseHdc($hdc);$graphics.Dispose()}
  $bitmap.Save($OutputPng,[Drawing.Imaging.ImageFormat]::Png);$bitmap.Dispose()
 }catch{$script:captureError=$_}finally{$form.Close()}
})
$form.Add_Shown({$timer.Start()})
try{[Windows.Forms.Application]::Run($form)}finally{$timer.Dispose();foreach($ico in $icons){$ico.Dispose()};$form.Dispose()}
if($captureError){throw $captureError}
