@echo OFF
REM Configure + build the Qt+VTK viewer (gmtvtk.dll + gmtvtk_demo.exe). Run from anywhere.
REM vcvars calls vswhere without a full path; put the Installer dir on PATH so it resolves.
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set CMAKE="C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
%CMAKE% -S "%~dp0." -B "%~dp0build" -G Ninja ^
    -DCMAKE_MAKE_PROGRAM=c:/j/bin/ninja.exe ^
    -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
%CMAKE% --build "%~dp0build"
