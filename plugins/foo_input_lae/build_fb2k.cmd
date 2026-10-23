@echo off
rem SPDX-FileCopyrightText: 2026 mtq0605
rem SPDX-License-Identifier: Apache-2.0
rem usage: build_fb2k.cmd <foobar2000 SDK folder (SDK-2019-12-27, unpacked)> <output folder>
rem Builds foo_input_lae.dll (x86) for foobar2000 1.4 / 1.5 and 2.x 32-bit. The SDK code needs /d2notypeopt (SDK readme);
rem the LAE decoder (lae_api.cpp) is compiled with its own verified flags (/fp:precise, no /d2notypeopt).
set SDK=%~1
set OUT=%~2
set LAE=%~dp0..\..\src
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
set COMMON=/nologo /c /O2 /EHsc /MT /std:c++17 /utf-8 /DUNICODE /D_UNICODE /DNDEBUG /DWIN32 /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS /W3 /wd4996 /wd4244 /wd4267 /wd4018 /wd4334
cl %COMMON% /d2notypeopt /I"%SDK%" /I"%SDK%\foobar2000" /Fo"%OUT%\obj\\" "%SDK%\foobar2000\SDK\*.cpp" "%SDK%\foobar2000\foobar2000_component_client\component_client.cpp" "%SDK%\pfc\*.cpp" > "%OUT%\build_sdk.txt" 2>&1
if errorlevel 1 (echo BUILD FAILED sdk & type "%OUT%\build_sdk.txt" & exit /b 1)
cl %COMMON% /d2notypeopt /DLAE_STATIC /I"%SDK%" /I"%SDK%\foobar2000" /I"%LAE%" /Fo"%OUT%\obj\\" "%~dp0foo_input_lae.cpp" > "%OUT%\build_component.txt" 2>&1
if errorlevel 1 (echo BUILD FAILED component & type "%OUT%\build_component.txt" & exit /b 1)
cl %COMMON% /fp:precise /D_USE_MATH_DEFINES /DLAE_STATIC /Fo"%OUT%\obj\\" "%LAE%\lae_api.cpp" > "%OUT%\build_lae.txt" 2>&1
if errorlevel 1 (echo BUILD FAILED liblae & type "%OUT%\build_lae.txt" & exit /b 1)
del /q "%OUT%\obj\pfc-fb2k-hooks.obj" "%OUT%\obj\nix-objects.obj" 2>nul
rem   (as in the SDK's "Release FB2K" configuration: these two pfc files are not part of a component)
link /nologo /DLL /OUT:"%OUT%\foo_input_lae.dll" "%OUT%\obj\*.obj" "%SDK%\foobar2000\shared\shared.lib" user32.lib shell32.lib ole32.lib uuid.lib advapi32.lib comctl32.lib gdi32.lib shlwapi.lib > "%OUT%\build_link.txt" 2>&1
if errorlevel 1 (echo LINK FAILED & type "%OUT%\build_link.txt" & exit /b 1)
echo built %OUT%\foo_input_lae.dll
