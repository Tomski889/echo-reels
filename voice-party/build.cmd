@echo off
setlocal
cd /d "%~dp0.."
rem Called after vcvars64 by the repository's main build.cmd.
if not exist dist\EchoParty mkdir dist\EchoParty
.venv\Scripts\python -m PyInstaller --noconfirm --windowed --onedir --exclude-module numpy --name PartyHost --distpath build\party-host-dist --workpath build\party-host --specpath build voice-party\party_host.py
if errorlevel 1 exit /b 1
xcopy /E /I /Y build\party-host-dist\PartyHost dist\EchoParty >nul
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /LD voice-party\bridge_sync.cpp /Fe:dist\EchoParty\BridgeSync.dll /Fo:build\obj\ /link /IMPLIB:build\obj\BridgeSync.lib
if errorlevel 1 exit /b 1
xcopy /E /I /Y voice-party\app dist\EchoParty\app >nul
copy /Y voice-party\party.json dist\EchoParty\party.json >nul
rem Node 22+ must be installed. Copy its runtime and license for the local demo.
if not exist dist\EchoParty\runtime mkdir dist\EchoParty\runtime
for /f "delims=" %%n in ('where node.exe') do if not exist dist\EchoParty\runtime\node.exe copy /Y "%%n" dist\EchoParty\runtime\node.exe >nul
if not exist dist\EchoParty\runtime\node.exe (echo ERROR: Node.js 22+ required & exit /b 1)
if not defined NODE_LICENSE (echo ERROR: set NODE_LICENSE to your official Node distribution LICENSE & exit /b 1)
copy /Y "%NODE_LICENSE%" dist\EchoParty\runtime\Node-LICENSE.txt >nul
exit /b %errorlevel%
