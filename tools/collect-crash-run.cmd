@echo off
rem One-click collector for the Vancouver 2010 / XLiveLessNess invite-accept crash.
rem Double-click this file. It only reads; the game folder is never modified.
rem It writes vancouver-crash-<timestamp>.zip to the Desktop.
setlocal
set "HERE=%~dp0"
echo Collecting crash evidence.
echo A Windows Error Reporting dump can be large, so this may take a minute.
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%collect-crash-run.ps1" %*
set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" echo The collector reported error code %RC%.
echo Press any key to close this window.
pause >nul
exit /b %RC%
