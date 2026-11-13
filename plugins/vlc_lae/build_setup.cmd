@echo off
rem SPDX-FileCopyrightText: 2026 mtq0605
rem SPDX-License-Identifier: Apache-2.0
rem usage: build_setup.cmd <VLC sdk folder> <output folder> [en | zh]   builds the module (build_vlc.cmd) and lae_vlc_setup.exe with the module inside
setlocal
set SDK=%~1
set OUT=%~2
set LANGDEF=
if /i "%~3"=="zh" set LANGDEF=/DLAE_LANG_ZH
if not "%~3"=="" if /i not "%~3"=="zh" if /i not "%~3"=="en" (echo language must be en or zh & exit /b 1)
if "%OUT%"=="" (echo usage: build_setup.cmd ^<VLC sdk folder^> ^<output folder^> & exit /b 1)
set HERE=%~dp0
call "%HERE%build_vlc.cmd" "%SDK%" "%OUT%" %~3 || exit /b 1
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
copy /y "%OUT%\liblae_plugin.dll" "%OUT%\obj\liblae_plugin.dll" >nul
copy /y "%HERE%setup\setup.manifest" "%OUT%\obj\setup.manifest" >nul
copy /y "%HERE%setup\setup.rc" "%OUT%\obj\setup.rc" >nul
rc /nologo /fo "%OUT%\obj\setup.res" "%OUT%\obj\setup.rc" >"%OUT%\build_rc.txt" 2>&1 || (type "%OUT%\build_rc.txt" & echo RC FAILED & exit /b 1)
cl /nologo /O2 /EHsc /std:c++17 /utf-8 %LANGDEF% /MT /DUNICODE /D_UNICODE /Fo"%OUT%\obj\setup.obj" /Fe"%OUT%\lae_vlc_setup.exe" "%HERE%setup\setup.cpp" "%OUT%\obj\setup.res" user32.lib advapi32.lib shell32.lib ole32.lib /link /SUBSYSTEM:WINDOWS >"%OUT%\build_setup.txt" 2>&1 || (type "%OUT%\build_setup.txt" & echo BUILD FAILED setup & exit /b 1)
echo built %OUT%\lae_vlc_setup.exe
exit /b 0
