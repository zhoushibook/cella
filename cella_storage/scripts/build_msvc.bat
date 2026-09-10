@echo off
REM cella_storage 存储系统 - MSVC/Ninja 一键构建脚本（Windows）
REM 路径全部基于脚本自身位置推导（%~dp0 = scripts 目录），
REM 改仓库文件夹名 / 挪动位置都不需要改本脚本。
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "ROOT=%~dp0.."
%CMAKE% -S "%ROOT%" -B "%ROOT%\build_ninja" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_CXX_COMPILER=cl
if errorlevel 1 exit /b 1
%CMAKE% --build "%ROOT%\build_ninja"
