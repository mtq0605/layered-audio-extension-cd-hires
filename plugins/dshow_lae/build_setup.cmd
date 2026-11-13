@echo off
rem SPDX-FileCopyrightText: 2026 mtq0605
rem SPDX-License-Identifier: Apache-2.0
rem usage: build_setup.cmd <output folder> [en | zh]   builds the filter (build_dshow.cmd) and lae_dshow_setup.exe with the filter inside
setlocal
set OUT=%~1
set LANGDEF=
if /i "%~2"=="zh" set LANGDEF=/DLAE_LANG_ZH
if not "%~2"=="" if /i not "%~2"=="zh" if /i not "%~2"=="en" (echo language must be en or zh & exit /b 1)
if "%OUT%"=="" (echo usage: build_setup.cmd ^<output folder^> & exit /b 1)
set HERE=%~dp0
call "%HERE%build_dshow.cmd" "%OUT%" %~2 || exit /b 1
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
copy /y "%OUT%\lae_source.ax" "%OUT%\obj\lae_source.ax" >nul
copy /y "%HERE%setup\setup.manifest" "%OUT%\obj\setup.manifest" >nul
copy /y "%HERE%setup\setup.rc" "%OUT%\obj\setup.rc" >nul
rc /nologo /fo "%OUT%\obj\setup.res" "%OUT%\obj\setup.rc" >"%OUT%\build_rc.txt" 2>&1 || (type "%OUT%\build_rc.txt" & echo RC FAILED & exit /b 1)
cl /nologo /O2 /EHsc /std:c++17 /utf-8 %LANGDEF% /MT /DUNICODE /D_UNICODE /Fo"%OUT%\obj\setup.obj" /Fe"%OUT%\lae_dshow_setup.exe" "%HERE%setup\setup.cpp" "%OUT%\obj\setup.res" user32.lib advapi32.lib shell32.lib ole32.lib /link /SUBSYSTEM:WINDOWS >"%OUT%\build_setup.txt" 2>&1 || (type "%OUT%\build_setup.txt" & echo BUILD FAILED setup & exit /b 1)
echo built %OUT%\lae_dshow_setup.exe
exit /b 0
