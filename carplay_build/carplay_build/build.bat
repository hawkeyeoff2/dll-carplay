@echo off
setlocal
cd /d "%~dp0"
echo === CarPlay ETS2 : compilation automatique ===

REM ---------- 1) Compilateur ----------
where cl >nul 2>nul
if not errorlevel 1 goto have_cl

:find_vs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if not exist "%VSWHERE%" goto no_vs
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto no_vs
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
goto have_cl

:no_vs
echo.
echo Le compilateur C++ de Microsoft n'est pas installe.
choice /m "Veux-tu l'installer automatiquement (Build Tools, plusieurs Go, via winget)"
if errorlevel 2 goto manual_vs
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
goto find_vs

:manual_vs
echo Installe "Visual Studio Build Tools" (coche "Developpement Desktop en C++") puis relance ce fichier.
pause
exit /b 1

:have_cl

REM ---------- 2) SDK de telemetrie SCS (telechargement auto) ----------
set "SDK_DIR=%~dp0scs_sdk"
set "INC="
if exist "%SDK_DIR%" for /f "delims=" %%f in ('dir /s /b "%SDK_DIR%\scssdk_telemetry.h" 2^>nul') do set "INC=%%~dpf"
if defined INC goto have_sdk

echo Telechargement du SDK de telemetrie SCS...
powershell -NoProfile -ExecutionPolicy Bypass -Command "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; Invoke-WebRequest 'https://github.com/Lihis/scs-sdk/archive/HEAD.zip' -OutFile 'scs_sdk.zip' -UseBasicParsing; Expand-Archive -Force 'scs_sdk.zip' 'scs_sdk'"
if exist "%SDK_DIR%" for /f "delims=" %%f in ('dir /s /b "%SDK_DIR%\scssdk_telemetry.h" 2^>nul') do set "INC=%%~dpf"
if defined INC goto have_sdk

echo.
echo ECHEC du telechargement du SDK (pas d'internet ?).
echo Telecharge-le a la main sur https://github.com/Lihis/scs-sdk (Code ^> Download ZIP)
echo et dezippe-le dans le dossier "scs_sdk" a cote de ce fichier, puis relance.
pause
exit /b 1

:have_sdk
echo SDK trouve : %INC%

REM ---------- 3) Compilation ----------
cl /nologo /TP /O2 /LD /W3 /I"%INC%." carplay_plugin.c user32.lib ws2_32.lib /Fe:carplay_plugin.dll /link /DEF:carplay_plugin.def
if errorlevel 1 goto fail
echo.
echo OK : carplay_plugin.dll cree.

REM ---------- 4) Installation auto si ETS2 est au chemin Steam par defaut ----------
set "PLUG=%ProgramFiles(x86)%\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\plugins"
if not exist "%ProgramFiles(x86)%\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64" goto manual_copy
if not exist "%PLUG%" mkdir "%PLUG%"
copy /y carplay_plugin.dll "%PLUG%\" >nul
copy /y carplay_plugin.ini "%PLUG%\" >nul
copy /y carplay.html "%PLUG%\" >nul
echo Installe dans : %PLUG%
goto done

:manual_copy
echo Copie carplay_plugin.dll, carplay_plugin.ini et carplay.html dans <ETS2>\bin\win_x64\plugins\

:done
pause
exit /b 0

:fail
echo.
echo ECHEC de la compilation : copie les erreurs ci-dessus et envoie-les moi.
pause
exit /b 1
