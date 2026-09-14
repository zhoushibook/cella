@echo off
REM vcbuild.bat -- build cella with the MSVC toolchain, without vcvars64.bat.
REM
REM Why not vcvars64.bat: it shells out to reg.exe, which the sandbox blacklists.
REM Instead we set INCLUDE / LIB / PATH directly from the known VS + Windows SDK
REM layout. Paths are stable for this machine's single VS 2022 + SDK 10.0.26100.
REM
REM Usage:  cmd //c vcbuild.bat [target]

setlocal enabledelayedexpansion

set "VCTOOLS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.43.34808"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.26100.0"

set "INCLUDE=%VCTOOLS%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\winrt;%SDK%\Include\%SDKVER%\cppwinrt"
set "LIB=%VCTOOLS%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"
set "PATH=%VCTOOLS%\bin\Hostx64\x64;%PATH%"

set "CMAKE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "BUILDDIR=C:\Users\Lenovo\Desktop\DBMS\cella\build"
set "LOG=%BUILDDIR%\vc_build.log"

if "%~1"=="" (
  "%CMAKE%" --build "%BUILDDIR%" > "%LOG%" 2>&1
) else (
  "%CMAKE%" --build "%BUILDDIR%" --target %1 > "%LOG%" 2>&1
)
set RC=%ERRORLEVEL%
echo EXIT %RC%
exit /b %RC%
