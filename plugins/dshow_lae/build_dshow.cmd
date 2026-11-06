@echo off
rem SPDX-FileCopyrightText: 2026 mtq0605
rem SPDX-License-Identifier: Apache-2.0
rem usage: build_dshow.cmd <output folder>   builds lae_source.ax (DirectShow source filter, Windows x64)
setlocal
set OUT=%~1
if "%OUT%"=="" (echo usage: build_dshow.cmd ^<output folder^> & exit /b 1)
set HERE=%~dp0
set LAE=%HERE%..\..\src
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl /nologo /c /O2 /EHsc /std:c++17 /MT /fp:precise /Fo"%OUT%\obj\lae_api.obj" "%LAE%\lae_api.cpp" >"%OUT%\build_lae.txt" 2>&1 || (type "%OUT%\build_lae.txt" & echo BUILD FAILED lae_api & exit /b 1)
cl /nologo /c /O2 /EHsc /std:c++17 /MT /fp:strict /Fo"%OUT%\obj\laes_api.obj" "%LAE%\laes_api.cpp" >"%OUT%\build_laes.txt" 2>&1 || (type "%OUT%\build_laes.txt" & echo BUILD FAILED laes_api & exit /b 1)
cl /nologo /c /O2 /EHsc /std:c++17 /MT /DUNICODE /D_UNICODE /DNOMINMAX /Fo"%OUT%\obj\lae_source.obj" "%HERE%src\lae_source.cpp" >"%OUT%\build_filter.txt" 2>&1 || (type "%OUT%\build_filter.txt" & echo BUILD FAILED filter & exit /b 1)
link /nologo /DLL /DEF:"%HERE%lae_source.def" /OUT:"%OUT%\lae_source.ax" "%OUT%\obj\lae_source.obj" "%OUT%\obj\lae_api.obj" "%OUT%\obj\laes_api.obj" strmiids.lib ole32.lib oleaut32.lib advapi32.lib user32.lib comctl32.lib >"%OUT%\build_link.txt" 2>&1 || (type "%OUT%\build_link.txt" & echo LINK FAILED & exit /b 1)
del /q "%OUT%\lae_source.exp" "%OUT%\lae_source.lib" 2>nul
echo built %OUT%\lae_source.ax
exit /b 0
