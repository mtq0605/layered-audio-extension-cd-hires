@echo off
rem SPDX-FileCopyrightText: 2026 mtq0605
rem SPDX-License-Identifier: Apache-2.0
rem usage: build_vlc.cmd <VLC sdk folder> <output folder> [en | zh]   (zh: the Chinese build; the two are standalone versions)
rem Builds liblae_plugin.dll (Windows x64) for VLC 3.0. The VLC sdk folder comes with VLC's 64-bit 7z package (vlc-3.0.x-win64.7z, folder
rem sdk; LGPL-2.1-or-later) and is not part of this repository. The LAE decoders are compiled with their own verified flags
rem (lae_api /fp:precise, laes_api /fp:strict).
setlocal
set SDK=%~1
set OUT=%~2
set LANGDEF=
if /i "%~3"=="zh" set LANGDEF=/DLAE_LANG_ZH
if not "%~3"=="" if /i not "%~3"=="zh" if /i not "%~3"=="en" (echo language must be en or zh & exit /b 1)
if "%OUT%"=="" (echo usage: build_vlc.cmd ^<VLC sdk folder^> ^<output folder^> & exit /b 1)
set HERE=%~dp0
set LAE=%HERE%..\..\src
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl /nologo /c /O2 /EHsc /std:c++17 /utf-8 /MD /fp:precise /Fo"%OUT%\obj\lae_api.obj" "%LAE%\lae_api.cpp" >"%OUT%\build_lae.txt" 2>&1 || (type "%OUT%\build_lae.txt" & echo BUILD FAILED lae_api & exit /b 1)
cl /nologo /c /O2 /EHsc /std:c++17 /utf-8 /MD /fp:strict /Fo"%OUT%\obj\laes_api.obj" "%LAE%\laes_api.cpp" >"%OUT%\build_laes.txt" 2>&1 || (type "%OUT%\build_laes.txt" & echo BUILD FAILED laes_api & exit /b 1)
cl /nologo /c /O2 /EHsc /std:c++17 /utf-8 %LANGDEF% /MD /Zc:preprocessor /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"%SDK%\include\vlc\plugins" /Fo"%OUT%\obj\lae_vlc.obj" "%HERE%src\lae_vlc.cpp" >"%OUT%\build_vlc.txt" 2>&1 || (type "%OUT%\build_vlc.txt" & echo BUILD FAILED module & exit /b 1)
link /nologo /DLL /OUT:"%OUT%\liblae_plugin.dll" "%OUT%\obj\lae_vlc.obj" "%OUT%\obj\lae_api.obj" "%OUT%\obj\laes_api.obj" "%SDK%\lib\libvlccore.lib" ws2_32.lib >"%OUT%\build_link.txt" 2>&1 || (type "%OUT%\build_link.txt" & echo LINK FAILED & exit /b 1)
del /q "%OUT%\liblae_plugin.exp" "%OUT%\liblae_plugin.lib" 2>nul
echo built %OUT%\liblae_plugin.dll
exit /b 0
