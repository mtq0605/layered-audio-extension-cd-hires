@echo off
rem SPDX-FileCopyrightText: 2026 mtq0605
rem SPDX-License-Identifier: Apache-2.0
rem usage: build_fb2k.cmd <SDK-2019-12-27 folder> <SDK-2026-10-01 folder> <output folder> [en | zh]
rem   en (default): the English build; zh: the Chinese build (/DLAE_LANG_ZH). The two are standalone versions.
rem Builds foo_input_lae.dll twice and packs both into foo_input_lae.fb2k-component (foobar2000 picks the one that fits):
rem   x86 (root of the package): foobar2000 1.4 / 1.5 and 2.x 32-bit, built against SDK-2019-12-27 (the SDK code needs /d2notypeopt);
rem   x64 (folder x64 in the package): foobar2000 2.x 64-bit, built against SDK-2026-10-01.
rem The LAE decoder (lae_api.cpp) is compiled with its own verified flags (/fp:precise, no /d2notypeopt), the LAES decoder
rem (laes_api.cpp) with /fp:strict (its reference taps are computed in double). Both SDKs are BSD-style licensed and not part of this repository.
setlocal
set SDK86=%~1
set SDK64=%~2
set OUT=%~3
set LANGDEF=
if /i "%~4"=="zh" set LANGDEF=/DLAE_LANG_ZH
if not "%~4"=="" if /i not "%~4"=="zh" if /i not "%~4"=="en" (echo language must be en or zh & exit /b 1)
if "%OUT%"=="" (echo usage: build_fb2k.cmd ^<SDK-2019-12-27^> ^<SDK-2026-10-01^> ^<output folder^> & exit /b 1)
set LAE=%~dp0..\..\src
set VC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build
call :one x86 "%SDK86%" vcvars32.bat shared.lib || exit /b 1
call :one x64 "%SDK64%" vcvars64.bat shared-x64.lib || exit /b 1
if exist "%OUT%\pkg" rmdir /s /q "%OUT%\pkg"
mkdir "%OUT%\pkg\x64"
copy /y "%OUT%\x86\foo_input_lae.dll" "%OUT%\pkg\" >nul
copy /y "%OUT%\x64\foo_input_lae.dll" "%OUT%\pkg\x64\" >nul
if exist "%OUT%\foo_input_lae.fb2k-component" del /q "%OUT%\foo_input_lae.fb2k-component"
rem tar.exe of Windows 10 1803+ (bsdtar) writes ZIP entry names with forward slashes (APPNOTE 4.4.17); Compress-Archive of
rem Windows PowerShell 5.1 writes backslashes, which unzip and other non-Windows readers take as part of one file name.
if exist "%OUT%\foo_input_lae.zip" del /q "%OUT%\foo_input_lae.zip"
"%SystemRoot%\System32\tar.exe" -a -c -f "%OUT%\foo_input_lae.zip" -C "%OUT%\pkg" foo_input_lae.dll x64 || (echo PACKING FAILED & exit /b 1)
move /y "%OUT%\foo_input_lae.zip" "%OUT%\foo_input_lae.fb2k-component" >nul
if errorlevel 1 (echo PACKING FAILED & exit /b 1)
rmdir /s /q "%OUT%\pkg"
echo built %OUT%\foo_input_lae.fb2k-component (x86 + x64)
exit /b 0

:one
setlocal
set ARCH=%~1
set SDK=%~2
set O=%OUT%\%ARCH%
call "%VC%\%~3" >nul 2>nul
if not exist "%O%\obj" mkdir "%O%\obj"
set STD=/std:c++17
if "%ARCH%"=="x64" set STD=/std:c++20
set COMMON=/nologo /c /O2 /EHsc /MT %STD% /utf-8 /DUNICODE /D_UNICODE /DNDEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS /W3 /wd4996 /wd4244 /wd4267 /wd4018 /wd4334
if "%ARCH%"=="x86" set COMMON=%COMMON% /DWIN32
cl %COMMON% /d2notypeopt /I"%SDK%" /I"%SDK%\foobar2000" /Fo"%O%\obj\\" "%SDK%\foobar2000\SDK\*.cpp" "%SDK%\foobar2000\foobar2000_component_client\component_client.cpp" "%SDK%\pfc\*.cpp" > "%O%\build_sdk.txt" 2>&1
if errorlevel 1 (echo BUILD FAILED sdk %ARCH% & type "%O%\build_sdk.txt" & exit /b 1)
set PERM=
if "%ARCH%"=="x64" set PERM=/permissive
cl %COMMON% %PERM% %LANGDEF% /d2notypeopt /DLAE_STATIC /I"%SDK%" /I"%SDK%\foobar2000" /I"%LAE%" /Fo"%O%\obj\\" "%~dp0foo_input_lae.cpp" > "%O%\build_component.txt" 2>&1
if errorlevel 1 (echo BUILD FAILED component %ARCH% & type "%O%\build_component.txt" & exit /b 1)
cl %COMMON% /std:c++17 /fp:precise /D_USE_MATH_DEFINES /DLAE_STATIC /Fo"%O%\obj\\" "%LAE%\lae_api.cpp" > "%O%\build_lae.txt" 2>&1
if errorlevel 1 (echo BUILD FAILED liblae %ARCH% & type "%O%\build_lae.txt" & exit /b 1)
cl %COMMON% /std:c++17 /fp:strict /D_USE_MATH_DEFINES /DLAE_STATIC /Fo"%O%\obj\\" "%LAE%\laes_api.cpp" > "%O%\build_laes.txt" 2>&1
if errorlevel 1 (echo BUILD FAILED liblae LAES %ARCH% & type "%O%\build_laes.txt" & exit /b 1)
del /q "%O%\obj\pfc-fb2k-hooks.obj" "%O%\obj\nix-objects.obj" 2>nul
rem   (as in the SDK's "Release FB2K" configuration: these two pfc files are not part of a component)
link /nologo /DLL /OUT:"%O%\foo_input_lae.dll" "%O%\obj\*.obj" "%SDK%\foobar2000\shared\%~4" user32.lib shell32.lib ole32.lib uuid.lib advapi32.lib comctl32.lib gdi32.lib shlwapi.lib > "%O%\build_link.txt" 2>&1
if errorlevel 1 (echo LINK FAILED %ARCH% & type "%O%\build_link.txt" & exit /b 1)
echo built %O%\foo_input_lae.dll
exit /b 0
