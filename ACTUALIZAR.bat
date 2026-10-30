@echo off
rem ===========================================================================================
rem  ACTUALIZAR.bat - actualiza TODO con un doble clic, sin preguntas:
rem   1. cierra los programas Kuang (instalador, Frame Extrapolation, Optimizer)
rem   2. borra build\ y dist\ (compilacion limpia: no queda nada viejo)
rem   3. compila todo desde cero (mods, cargador, programas y el propio instalador)
rem   4. abre KuangInstaller.exe en modo automatico: cierra Geometry Dash (normal, para que guarde),
rem      instala los mods en el juego, comprueba byte por byte y se cierra solo si todo salio bien
rem  Tus ajustes (kfe.ini, kopt.ini, kmenu.ini, kmenu-smartsp.txt) no se tocan.
rem ===========================================================================================
cd /d "%~dp0"

rem administrador: el Optimizer corre como administrador y hay que poder cerrarlo
net session >nul 2>&1
if errorlevel 1 (
    echo Pidiendo permisos de administrador...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

call :main
set RC=%ERRORLEVEL%
if not "%RC%"=="0" (
    echo.
    echo ERROR %RC%: algo fallo, lee las lineas de arriba.
    pause
    exit /b %RC%
)
echo.
echo [4/4] Instalando en Geometry Dash (se cierra solo al terminar)...
start "" "%~dp0KuangInstaller.exe" --auto --from-build
exit /b 0

:main
echo [1/4] Cerrando los programas Kuang...
for %%p in (KuangInstaller.exe KuangFrameExtrapolation.exe KuangOptimizer.exe) do taskkill /IM %%p >nul 2>&1
timeout /t 2 /nobreak >nul
for %%p in (KuangInstaller.exe KuangFrameExtrapolation.exe KuangOptimizer.exe) do taskkill /F /IM %%p >nul 2>&1

echo [2/4] Borrando build\ y dist\ ...
if exist build rmdir /s /q build
if exist dist rmdir /s /q dist
if exist build (echo No se pudo borrar build\: cierra Visual Studio o el explorador que lo tenga abierto. & exit /b 15)
if exist dist (echo No se pudo borrar dist\: algun programa de dist\ sigue abierto. & exit /b 16)

for /f "tokens=2,*" %%a in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v Path 2^>nul') do set "MPATH=%%b"
for /f "tokens=2,*" %%a in ('reg query HKCU\Environment /v Path 2^>nul') do set "UPATH=%%b"
call set "PATH=%MPATH%;%UPATH%;%ProgramFiles%\CMake\bin;%PATH%"
where cmake >nul 2>&1
if errorlevel 1 (echo No encuentro CMake: ejecuta instalar-herramientas.bat una vez. & exit /b 10)

echo [3/4] Compilando todo desde cero (tarda un poco)...
cmake -S . -B build -A x64 -T ClangCL
if errorlevel 1 exit /b 20
cmake --build build --config Release
if errorlevel 1 exit /b 30
for %%f in (kfe.dll kmenu.dll kopt.dll winmm.dll KuangFrameExtrapolation.exe KuangOptimizer.exe) do if not exist "build\Release\%%f" (echo FALTA build\Release\%%f - no se compilo o el antivirus lo quito & exit /b 40)
if not exist "KuangInstaller.exe" (echo FALTA KuangInstaller.exe & exit /b 41)
echo Compilado.
exit /b 0
