@echo OFF
setlocal EnableDelayedExpansion
REM Configure + build the Qt+VTK viewer (gmtvtk.dll, gmtvtk_test.dll, gmtvtk_demo.exe). Run from
REM anywhere -- every path below is derived from %~dp0 or discovered, never assumed.
REM
REM NOTHING HERE IS PINNED TO ONE MACHINE. The previous version hard-coded "Visual Studio\18\
REM Community", the cmake.exe inside it, and c:/j/bin/ninja.exe. On any other box all three are
REM wrong. Visual Studio is now located with vswhere (the one tool Microsoft guarantees at a fixed
REM path), and cmake/ninja are taken from PATH when present.
REM
REM It also called vcvars64 with vswhere NOT on PATH -- the line that put it there was commented
REM out -- so every single run printed
REM     'vswhere.exe' is not recognized as an internal or external command
REM before carrying on. That is why this looked like it "always errors".

REM ---------------------------------------------------------------- Visual Studio
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [build] ERROR: vswhere.exe not found. Is Visual Studio installed?
    exit /b 1
)
REM vcvars64.bat itself shells out to vswhere by bare name, so it must be on PATH too.
for %%D in ("%VSWHERE%") do set "PATH=%%~dpD;%PATH%"

set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo [build] ERROR: no Visual Studio with the C++ toolset ^(VC.Tools.x86.x64^) was found.
    exit /b 1
)
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (
    echo [build] ERROR: "%VSDIR%" has no VC\Auxiliary\Build\vcvars64.bat
    exit /b 1
)
echo [build] Visual Studio: %VSDIR%
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo [build] ERROR: vcvars64.bat failed. & exit /b 1 )

REM ---------------------------------------------------------------- cmake + ninja
REM PATH first, so a developer's own install wins; the one bundled with Visual Studio is the fallback.
set "CMAKE="
for /f "delims=" %%i in ('where cmake 2^>nul') do if not defined CMAKE set "CMAKE=%%i"
if not defined CMAKE set "CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" ( echo [build] ERROR: cmake.exe not found ^(not on PATH, not in Visual Studio^). & exit /b 1 )

set "NINJA="
for /f "delims=" %%i in ('where ninja 2^>nul') do if not defined NINJA set "NINJA=%%i"
if not defined NINJA if exist "%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" set "NINJA=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not defined NINJA if exist "c:\j\bin\ninja.exe" set "NINJA=c:\j\bin\ninja.exe"
if not defined NINJA ( echo [build] ERROR: ninja.exe not found ^(not on PATH, not in Visual Studio^). & exit /b 1 )
echo [build] cmake: %CMAKE%
echo [build] ninja: %NINJA%

REM ---------------------------------------------------------------- configure
"%CMAKE%" -S "%~dp0." -B "%~dp0build" -G Ninja ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
    -DCMAKE_BUILD_TYPE=Release %*
if errorlevel 1 ( echo [build] ERROR: configure failed. & exit /b 1 )

REM ---------------------------------------------------------------- build
REM "ninja: no work to do." is SUCCESS, not a failure: every output is already newer than every
REM source. Only deps/src/gmtvtk.cpp and deps/src/mbgrid.c are compiled -- the numbered fragments
REM (00_includes.cpp, 70_window.cpp, ...) are #included into gmtvtk.cpp -- so to force a rebuild,
REM touch deps/src/gmtvtk.cpp.
"%CMAKE%" --build "%~dp0build"
if not errorlevel 1 goto :done

REM A link that fails with LNK1104 "cannot open file 'gmtvtk.dll'" means the library is mapped by a
REM running process (an iGMT window, or a Julia session that has loaded it). Windows refuses to
REM overwrite a mapped file but DOES allow renaming it, which is the same displacement trick
REM deps/build.jl uses when it updates a locked dll. Displace and try once more.
echo [build] build failed -- retrying after displacing any locked library
set "STAMP=%RANDOM%%RANDOM%"
for %%F in (gmtvtk.dll gmtvtk_test.dll) do (
    if exist "%~dp0build\%%F" ren "%~dp0build\%%F" "%%F.old-!STAMP!" 2>nul
)
"%CMAKE%" --build "%~dp0build"
if errorlevel 1 (
    echo [build] ERROR: build failed. If it is still LNK1104, close any running iGMT window or
    echo [build]        Julia session that has gmtvtk.dll loaded, then run this again.
    exit /b 1
)

:done
REM Sweep the displaced copies from this run and any earlier one. A file still mapped by a live
REM process cannot be unlinked; skip it silently, the next run gets it.
del /q "%~dp0build\*.old-*" 2>nul
echo [build] OK
endlocal
exit /b 0
