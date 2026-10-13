@echo off
rem Build dinput.dll proxy (i686, no CRT) with clang + lld-link.
rem Output: src\proxy\dinput.dll
setlocal
cd /d %~dp0..\src\proxy

set SDK=C:\Program Files (x86)\Windows Kits\10
set VER=
for /f "delims=" %%v in ('dir /b /ad /o-n "%SDK%\Include\10.0.*" 2^>nul') do (
    if not defined VER set VER=%%v
)
if not defined VER (echo Windows SDK 10 not found & exit /b 1)
set INC=-I"%SDK%\Include\%VER%\um" -I"%SDK%\Include\%VER%\shared" -I"%SDK%\Include\%VER%\ucrt"
set LIB=/libpath:"%SDK%\Lib\%VER%\um\x86" /libpath:"%SDK%\Lib\%VER%\ucrt\x86"

clang --target=i686-pc-windows-msvc -O2 -fno-stack-protector %INC% -c dinput_proxy.c -o dinput_proxy.obj || exit /b 1
lld-link -dll -def:dinput.def -out:dinput.dll -entry:DllMainCRTStartup -nodefaultlib %LIB% dinput_proxy.obj kernel32.lib user32.lib gdi32.lib d3d9.lib || exit /b 1
echo Built dinput.dll
endlocal
