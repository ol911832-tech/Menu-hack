@echo off
rem Compila todos los mods Kuang (KFE, Optimizer, Menu, cargador, apps e instalador) en dist\ y build\Release\.
rem Para compilar E instalar en un solo paso, usa KuangInstaller.exe.
cd /d "%~dp0"
call :main > compilar.log 2>&1
set RC=%ERRORLEVEL%
type compilar.log
if "%RC%"=="0" (echo. & echo LISTO: abre KuangInstaller.exe para instalar todo en el juego) else (echo. & echo ERROR %RC%: revisa compilar.log)
pause
exit /b %RC%

:main
for /f "tokens=2,*" %%a in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v Path 2^>nul') do set "MPATH=%%b"
for /f "tokens=2,*" %%a in ('reg query HKCU\Environment /v Path 2^>nul') do set "UPATH=%%b"
call set "PATH=%MPATH%;%UPATH%;%ProgramFiles%\CMake\bin;%PATH%"
where cmake
if errorlevel 1 exit /b 10
echo [1/2] Preparando...
cmake -S . -B build -A x64 -T ClangCL
if errorlevel 1 exit /b 20
echo [2/2] Compilando...
cmake --build build --config Release
if errorlevel 1 exit /b 30
for %%f in (kfe.dll kmenu.dll kopt.dll winmm.dll KuangFrameExtrapolation.exe KuangOptimizer.exe) do if not exist "build\Release\%%f" (echo FALTA build\Release\%%f - no se compilo o el antivirus lo quito & exit /b 40)
if not exist "KuangInstaller.exe" exit /b 41
if not exist "KuangSetup.exe" exit /b 42
echo COMPILADO_OK
exit /b 0
