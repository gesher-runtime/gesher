@echo off
rem gesher-app <app.apk> [options]: runs the app's launcher Activity headless
rem (see gesher-app.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0gesher-app.ps1" %*
exit /b %ERRORLEVEL%
