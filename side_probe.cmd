@echo off
rem Side panel experiment on the TEST build: reinstalls Echo Arcade with the arcade screen also drawn where the
rem social tablet's player card sits (tools\build_arcade_tab.py, ECHO_ARCADE_SIDE_PROBE=1).
rem   side_probe.cmd        install the experiment
rem   side_probe.cmd off    back to the normal tab
rem Close Echo VR first. Replaces the install made by the Reels installer (if any) with this dev install.
setlocal
set "GAME=E:\tes\Software\ready-at-dawn-echo-arena"
set "DATA=%GAME%\_data\5932408047\rad15\win10"
set "INI=%GAME%\bin\win10\plugins\EchoArcade\arcade.ini"
cd /d "%~dp0"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

tasklist /FI "IMAGENAME eq echovr.exe" | find /I "echovr.exe" >nul && (echo Close Echo VR first. & exit /b 1)

if /I "%~1"=="off" (set "ECHO_ARCADE_SIDE_PROBE=") else (set "ECHO_ARCADE_SIDE_PROBE=1")

echo == 1. Removing the current Echo Arcade install
if exist "%LOCALAPPDATA%\EchoArcadeReels\app\install_state.json" (
  pushd "%LOCALAPPDATA%\EchoArcadeReels\app"
  "%LOCALAPPDATA%\EchoArcadeReels\app\python\python.exe" -u tools\install.py restore --force --game "%GAME%" || (popd & exit /b 1)
  popd
)
if exist "%~dp0install_state.json" (
  "%~dp0.venv\Scripts\python" "%~dp0tools\install.py" restore --force --game "%GAME%" || exit /b 1
)

echo == 2. Building and installing the tablet tab
"%~dp0.venv\Scripts\python" "%~dp0tools\install.py" install --game "%GAME%" || exit /b 1

echo == 3. Rebuilding the plugin for the new tab layout
call "%~dp0build.cmd" || exit /b 1
"%~dp0.venv\Scripts\python" "%~dp0tools\install.py" update --game "%GAME%" || exit /b 1

echo == 4. Reels settings (REELS + CAMERA tiles, sound on the Windows default output)
powershell -NoProfile -Command "$f='%INI%'; $t=[IO.File]::ReadAllLines($f) | Where-Object { $_ -notmatch '^(tiles|audio_device)=' }; $i=[Array]::IndexOf($t,'[host]'); $t=@($t[0..$i]) + 'tiles=reels,camera' + 'audio_device=default' + @($t[($i+1)..($t.Length-1)]); [IO.File]::WriteAllLines($f,$t)"

echo == 5. Re-applying the Echo Restoration texture patch
python "E:\echo halloween\tools\evr_patch.py" textures "%DATA%" || exit /b 1

echo.
if defined ECHO_ARCADE_SIDE_PROBE (echo Done: experiment installed. Start Echo, open the tablet and look to the RIGHT of it.) else (echo Done: normal tab installed.)
