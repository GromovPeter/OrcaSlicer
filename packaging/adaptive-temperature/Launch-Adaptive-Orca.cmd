@echo off
setlocal
set "ADAPTIVE_ORCA_DATA=%LOCALAPPDATA%\OrcaSlicer-Adaptive-Temperature-Experimental"
if not exist "%ADAPTIVE_ORCA_DATA%" mkdir "%ADAPTIVE_ORCA_DATA%"
set "WEBVIEW2_USER_DATA_FOLDER=%ADAPTIVE_ORCA_DATA%\WebView2"
pushd "%~dp0"
start "" /wait "%~dp0orca-slicer.exe" --datadir "%ADAPTIVE_ORCA_DATA%" %*
set "ADAPTIVE_ORCA_EXIT=%ERRORLEVEL%"
popd
exit /b %ADAPTIVE_ORCA_EXIT%
