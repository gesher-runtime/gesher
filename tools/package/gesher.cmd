@echo off
rem Gesher's command: gesher [file.dex class [args...]] (see gesher.ps1).
rem Without arguments it runs the bundled Hello and waits for a key, so the
rem Start menu shortcut leaves its window open.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0gesher.ps1" %*
set code=%ERRORLEVEL%
if "%~1"=="" pause
exit /b %code%
