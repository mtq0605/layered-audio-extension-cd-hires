@echo off
rem SPDX-FileCopyrightText: 2026 mtq0605
rem SPDX-License-Identifier: Apache-2.0
rem usage: build_kodi.cmd <kodi-dev-kit include folder> <output folder> [en | zh]
rem Builds audiodecoder.lae.dll (Windows x64) for Kodi 21 and packs audiodecoder.lae-<version>-<en|zh>.zip (install from zip file in Kodi).
rem en (default) and zh are standalone language versions: lang_pkg.ps1 keeps one language in the package.
rem The kodi-dev-kit headers come from Kodi's repository (xbmc/addons/kodi-dev-kit/include, tag 21.3-Omega; GPL-2.0-or-later) and are
rem not part of this repository. The LAE decoders are compiled with their own verified flags (lae_api /fp:precise, laes_api /fp:strict).
setlocal
set KDK=%~1
set OUT=%~2
set PKGLANG=en
if /i "%~3"=="zh" set PKGLANG=zh
if not "%~3"=="" if /i not "%~3"=="zh" if /i not "%~3"=="en" (echo language must be en or zh & exit /b 1)
if "%OUT%"=="" (echo usage: build_kodi.cmd ^<kodi-dev-kit include^> ^<output folder^> [en ^| zh] & exit /b 1)
set HERE=%~dp0
set LAE=%HERE%..\..\src
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl /nologo /c /O2 /EHsc /std:c++17 /utf-8 /MD /fp:precise /Fo"%OUT%\obj\lae_api.obj" "%LAE%\lae_api.cpp" >"%OUT%\build_lae.txt" 2>&1 || (type "%OUT%\build_lae.txt" & echo BUILD FAILED lae_api & exit /b 1)
cl /nologo /c /O2 /EHsc /std:c++17 /utf-8 /MD /fp:strict /Fo"%OUT%\obj\laes_api.obj" "%LAE%\laes_api.cpp" >"%OUT%\build_laes.txt" 2>&1 || (type "%OUT%\build_laes.txt" & echo BUILD FAILED laes_api & exit /b 1)
cl /nologo /c /O2 /EHsc /std:c++17 /utf-8 /MD /DBUILD_KODI_ADDON /DADDON_INSTANCE_VERSION_AUDIODECODER_USED /DADDON_GLOBAL_VERSION_GENERAL_USED /DADDON_GLOBAL_VERSION_GUI_USED /DADDON_GLOBAL_VERSION_AUDIOENGINE_USED /DADDON_GLOBAL_VERSION_FILESYSTEM_USED /DTARGET_WINDOWS /DNOMINMAX /D_USE_MATH_DEFINES /I"%KDK%" /Fo"%OUT%\obj\addon.obj" "%HERE%src\addon.cpp" >"%OUT%\build_addon.txt" 2>&1 || (type "%OUT%\build_addon.txt" & echo BUILD FAILED addon & exit /b 1)
if exist "%OUT%\pkg" rmdir /s /q "%OUT%\pkg"
xcopy /e /i /q "%HERE%audiodecoder.lae" "%OUT%\pkg\audiodecoder.lae" >nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%lang_pkg.ps1" -PkgDir "%OUT%\pkg\audiodecoder.lae" -Lang %PKGLANG% >nul || (echo LANGUAGE PACKAGE FAILED & exit /b 1)
link /nologo /DLL /OUT:"%OUT%\pkg\audiodecoder.lae\audiodecoder.lae.dll" "%OUT%\obj\addon.obj" "%OUT%\obj\lae_api.obj" "%OUT%\obj\laes_api.obj" >"%OUT%\build_link.txt" 2>&1 || (type "%OUT%\build_link.txt" & echo LINK FAILED & exit /b 1)
del /q "%OUT%\pkg\audiodecoder.lae\audiodecoder.lae.exp" "%OUT%\pkg\audiodecoder.lae\audiodecoder.lae.lib" 2>nul
rem the version: the 4th quote-separated field of the line <addon id="..." version="x.y.z" ...>
for /f tokens^=4^ delims^=^" %%v in ('findstr /b /c:"<addon id=" "%HERE%audiodecoder.lae\addon.xml"') do set V=%%v
if exist "%OUT%\audiodecoder.lae-%V%-%PKGLANG%.zip" del /q "%OUT%\audiodecoder.lae-%V%-%PKGLANG%.zip"
rem tar.exe of Windows 10 1803+ (bsdtar) writes ZIP entry names with forward slashes (APPNOTE 4.4.17); Compress-Archive of
rem Windows PowerShell 5.1 writes backslashes, which unzip and Kodi on Linux take as part of one file name.
"%SystemRoot%\System32\tar.exe" -a -c -f "%OUT%\audiodecoder.lae-%V%-%PKGLANG%.zip" -C "%OUT%\pkg" audiodecoder.lae
if errorlevel 1 (echo PACKING FAILED & exit /b 1)
echo built %OUT%\audiodecoder.lae-%V%-%PKGLANG%.zip
exit /b 0
