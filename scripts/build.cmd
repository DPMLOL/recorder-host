@echo off
setlocal EnableDelayedExpansion
REM Builds headless OBS and recorder-host into build\obs-install (runnable from bin\64bit).
REM
REM   scripts\build.cmd          build
REM   scripts\build.cmd --sign   + sign the binaries injected into or launched against the game (smctl, DigiCert KeyLocker)
REM
REM Env: DPM_SIGN_KEYPAIR (smctl keypair alias, needed with --sign)

set "ROOT=%~dp0.."
set "INSTALL=%ROOT%\build\obs-install"
set "SIGN=0"
for %%a in (%*) do if "%%a"=="--sign" set "SIGN=1"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
where cmake >nul 2>&1 && (set "CMAKE=cmake") || (
    for /f "delims=" %%p in ('"%VSWHERE%" -latest -find Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe') do set "CMAKE=%%p"
)
for /f "delims=" %%p in ('"%VSWHERE%" -latest -find VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe') do set "DUMPBIN=%%p"
if not defined CMAKE (echo ERROR: cmake not found & exit /b 1)
if not defined DUMPBIN (echo ERROR: dumpbin not found & exit /b 1)

echo [build] submodules
git -C "%ROOT%" submodule update --init --recursive --depth 1 || exit /b 1

echo [build] patches
for %%f in ("%ROOT%\patches\*.patch") do (
    git -C "%ROOT%\obs-studio" apply --reverse --check "%%f" >nul 2>&1
    if errorlevel 1 (
        git -C "%ROOT%\obs-studio" apply "%%f" || (echo ERROR: patch %%~nxf does not apply & exit /b 1)
        echo   applied %%~nxf
    ) else (
        echo   already applied %%~nxf
    )
)

echo [build] obs-studio
"%CMAKE%" -S "%ROOT%\obs-studio" --preset windows-x64 -B "%ROOT%\build\obs" -Wno-dev ^
    -DENABLE_FRONTEND=OFF -DENABLE_SCRIPTING=OFF -DENABLE_BROWSER=OFF -DENABLE_WEBSOCKET=OFF ^
    -DENABLE_AJA=OFF -DENABLE_VST=OFF || exit /b 1
"%CMAKE%" --build "%ROOT%\build\obs" --config RelWithDebInfo --parallel || exit /b 1
"%CMAKE%" --install "%ROOT%\build\obs" --config RelWithDebInfo --prefix "%INSTALL%" || exit /b 1
"%CMAKE%" --install "%ROOT%\build\obs" --config RelWithDebInfo --prefix "%INSTALL%" --component Development || exit /b 1

echo [build] recorder-host
"%CMAKE%" -S "%ROOT%" -B "%ROOT%\build\host" -G "Visual Studio 18 2026" -A x64 -Wno-dev || exit /b 1
"%CMAKE%" --build "%ROOT%\build\host" --config RelWithDebInfo || exit /b 1
"%CMAKE%" --install "%ROOT%\build\host" --config RelWithDebInfo --prefix "%INSTALL%" || exit /b 1

echo [build] runtime dependencies
REM obs-deps-20* and not obs-deps-*: the Qt package (obs-deps-qt6-*) would match too.
for /d %%d in ("%ROOT%\obs-studio\.deps\obs-deps-20*-x64") do set "DEPS=%%d\bin"
node "%ROOT%\scripts\collect-deps.mjs" "%INSTALL%" "%DEPS%" "%DUMPBIN%" || exit /b 1

if "%SIGN%"=="1" (
    if not defined DPM_SIGN_KEYPAIR (echo ERROR: set DPM_SIGN_KEYPAIR to the smctl keypair alias. & exit /b 1)
    set "HOOK=%INSTALL%\data\obs-plugins\win-capture"
    for %%f in ("!HOOK!\graphics-hook64.dll" "!HOOK!\graphics-hook32.dll" "!HOOK!\inject-helper64.exe" "!HOOK!\inject-helper32.exe" "!HOOK!\get-graphics-offsets64.exe" "!HOOK!\get-graphics-offsets32.exe" "%INSTALL%\bin\64bit\recorder-host.exe") do (
        REM --simple signs service-side; without it smctl needs the DigiCert KSP installed locally.
        smctl sign --simple --keypair-alias %DPM_SIGN_KEYPAIR% --input "%%~f" || (echo ERROR: signing %%~nxf failed & exit /b 1)
    )
)

echo [build] done: %INSTALL%\bin\64bit\recorder-host.exe
exit /b 0
