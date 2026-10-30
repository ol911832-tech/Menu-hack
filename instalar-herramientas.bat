@echo off
rem Instala lo necesario para compilar los mods Kuang (una sola vez):
rem Visual Studio Build Tools 2022 (C++ y Clang) y CMake. Sin Geode.
cd /d "%~dp0"
call :main
if errorlevel 1 (echo. & echo ERROR %ERRORLEVEL%) else (echo. & echo LISTO: ahora abre KuangInstaller.exe o haz doble clic en compilar.bat)
echo.
pause
exit /b

:main
where winget >nul 2>nul || (echo FALTA_WINGET: instala "App Installer" desde Microsoft Store & exit /b 10)
echo [1/2] Visual Studio Build Tools 2022 (C++ y Clang). Tarda bastante, ~6-8 GB...
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-package-agreements --accept-source-agreements --override "--passive --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset --add Microsoft.VisualStudio.Component.VC.CMake.Project"
echo [2/2] CMake...
winget install --id Kitware.CMake -e --accept-package-agreements --accept-source-agreements
echo INSTALACION_OK
exit /b 0
