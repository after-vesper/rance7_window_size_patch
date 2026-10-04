@echo off
rem Build the proxy DLL and assemble the distributable patch package:
rem   dist\SengokuRance_WindowSizePatch\       raw files for manual install
rem   dist\SengokuRance_WindowSizePatch.zip
rem   dist\SengokuRance_WindowSizePatch_Setup.exe  self-contained installer
setlocal
cd /d %~dp0..
call tools\build.bat || exit /b 1

rem --- installer (embeds the freshly built files) ---
python src\installer\gen_assets.py src\installer\embedded_assets.inc || exit /b 1

set SDK=C:\Program Files (x86)\Windows Kits\10
set VER=
for /f "delims=" %%v in ('dir /b /ad /o-n "%SDK%\Include\10.0.*" 2^>nul') do (
    if not defined VER set VER=%%v
)
if not defined VER (echo Windows SDK 10 not found & exit /b 1)
if not exist dist mkdir dist || exit /b 1
set INC=-I"%SDK%\Include\%VER%\um" -I"%SDK%\Include\%VER%\shared" -I"%SDK%\Include\%VER%\ucrt" -Isrc\installer
set LIB=/libpath:"%SDK%\Lib\%VER%\um\x86" /libpath:"%SDK%\Lib\%VER%\ucrt\x86"

clang --target=i686-pc-windows-msvc -O2 -fno-stack-protector -mno-stack-arg-probe %INC% -c src\installer\installer.c -o src\installer\installer.obj || exit /b 1
lld-link -subsystem:windows -manifest:embed "-manifestuac:level='requireAdministrator' uiAccess='false'" -out:dist\SengokuRance_WindowSizePatch_Setup.exe -entry:mainCRTStartup -nodefaultlib %LIB% src\installer\installer.obj kernel32.lib user32.lib advapi32.lib || exit /b 1
echo Built installer

rem --- manual-install package ---
set DIST=dist\SengokuRance_WindowSizePatch
if not exist "%DIST%" mkdir "%DIST%" || exit /b 1
copy /y src\proxy\dinput.dll "%DIST%\" >nul
copy /y src\proxy\WindowSize.ini "%DIST%\" >nul
copy /y README.md "%DIST%\README_WindowSizePatch.md" >nul || echo WARN: README missing

powershell -NoProfile -Command "Compress-Archive -Force -Path '%DIST%' -DestinationPath 'dist\SengokuRance_WindowSizePatch.zip'" || exit /b 1
echo Packaged to dist\
endlocal
