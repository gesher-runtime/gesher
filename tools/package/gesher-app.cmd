@echo off
rem gesher-app <app.apk> [options]: runs the app's launcher Activity headless
rem (native gesher-app.exe when present, else gesher-app.ps1).
if exist "%~dp0gesher-app.exe" goto :native
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0gesher-app.ps1" %*
exit /b %ERRORLEVEL%
:native
"%~dp0gesher-app.exe" %*
exit /b %ERRORLEVEL%
