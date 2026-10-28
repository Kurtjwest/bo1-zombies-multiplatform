@echo off
rem tools\pgo-train.bat [FOLDER] - the training of a profile-guided Windows build (build-windows.sh --pgo-generate):
rem the instrumented BO1Zombies.exe in FOLDER (with the game files next to it: setup.ps1 -Build FOLDER) plays Kino der
rem Toten and Five with its test bot, 2 minutes each in a window (keep it in front), and records what it ran into
rem *.profraw files in FOLDER. Then build-windows.sh --pgo-profile FOLDER builds the game with them.
rem FOLDER: build-windows-x64-pgo-gen next to this tools folder unless given.
setlocal
set "GAME=%~1"
if "%GAME%"=="" set "GAME=%~dp0..\build-windows-x64-pgo-gen"
if not exist "%GAME%\BO1Zombies.exe" (echo No instrumented game in %GAME% & exit /b 1)
cd /d "%GAME%"
set "GAME=%CD%"
set "LLVM_PROFILE_FILE=%GAME%\pgo-%%p-%%m.profraw"
for %%M in (zombie_theater zombie_pentagon) do (
    echo PGO training: %%M
    start "" /wait "%GAME%\BO1Zombies.exe" +set bo1_zombies 1 +set fs_b "%GAME%" +set fs_h "%GAME%" +set logfile 2 +set net_ip 127.0.0.1 +set r_fullscreen 0 +set bo1_testclient_realwindow 1 +set bo1_testclient 1 +set bo1_testclient_fight 1 +set bo1_testclient_god 1 +set bo1_testclient_seed 1 +set bo1_benchmark 1 +set bo1_benchmark_start 15000 +set bo1_benchmark_seconds 120 +set com_maxfps 0 +set r_vsync 0 +set bo1_autoquit 420000 +devmap %%M
)
dir /b "%GAME%\*.profraw" >nul 2>&1 || (echo No recording was written: did the game start? & exit /b 1)
echo Recorded. Now: ./build-windows.sh --pgo-profile "%GAME%"
