@echo off
rem Build the native core with zig on Windows (the PC has no bash for build.sh): build.cmd [outdir]
rem   <outdir>\librncore.a      every core module + yyjson, for native/ui to link
rem   <outdir>\rn-core-cli.exe  the command line alone (console subsystem), for tests
rem ZIG = path to zig.exe; RN_DEFINES as in build.sh. Same flags and archive members as build.sh, compiled one by one.
rem Another version: RN_VERSION_H = a version.gen.h made by version.sh (just build-pc writes it on the dev box: cmd has
rem no semver parser); it is copied to <outdir>\obj, where native\ui reads it too.
setlocal enabledelayedexpansion
cd /d "%~dp0"
if "%ZIG%"=="" set ZIG=zig
set OUT=%~1
if "%OUT%"=="" set OUT=out
set OBJ=%OUT%\obj
if exist "%OBJ%" rmdir /s /q "%OBJ%"
mkdir "%OBJ%" || exit /b 1
set CXXFLAGS=-target x86_64-windows-gnu -std=c++20 -Os -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Wall -Wno-unused-function -Wno-missing-braces %RN_DEFINES%
if not "%RN_VERSION%"=="" if "%RN_VERSION_H%"=="" (echo RN_VERSION needs RN_VERSION_H here: a header from native/core/version.sh & exit /b 1)
if not "%RN_VERSION_H%"=="" (
  copy /y "%RN_VERSION_H%" "%OBJ%\version.gen.h" >nul || exit /b 1
  set CXXFLAGS=%CXXFLAGS% -DRN_VERSION_GEN -I%OBJ%
)
set LIBS=-lwinhttp -lbcrypt -lole32 -loleaut32 -luuid -lshell32 -ladvapi32 -luser32
set LAUNCH=launch_stub
if exist launch.cpp set LAUNCH=launch
"%ZIG%" cc -target x86_64-windows-gnu -Os -ffunction-sections -fdata-sections -c vendor\yyjson\yyjson.c -o "%OBJ%\yyjson.o" || exit /b 1
set OBJS=
for %%f in (*.cpp) do (
  if /i not "%%~nf"=="cli_main" if /i not "%%~nf"=="launch" if /i not "%%~nf"=="launch_stub" (
    "%ZIG%" c++ %CXXFLAGS% -c %%f -o "%OBJ%\%%~nf.o" || exit /b 1
    set OBJS=!OBJS! "%OBJ%\%%~nf.o"
  )
)
"%ZIG%" c++ %CXXFLAGS% -c %LAUNCH%.cpp -o "%OBJ%\%LAUNCH%.o" || exit /b 1
"%ZIG%" c++ %CXXFLAGS% -c cli_main.cpp -o "%OBJ%\cli_main.o" || exit /b 1
if exist "%OUT%\librncore.a" del "%OUT%\librncore.a"
"%ZIG%" ar rcs "%OUT%\librncore.a" !OBJS! "%OBJ%\%LAUNCH%.o" "%OBJ%\yyjson.o" || exit /b 1
"%ZIG%" c++ %CXXFLAGS% -Wl,--subsystem,console -Wl,--gc-sections -s "%OBJ%\cli_main.o" "%OUT%\librncore.a" %LIBS% -o "%OUT%\rn-core-cli.exe" || exit /b 1
for %%f in ("%OUT%\librncore.a" "%OUT%\rn-core-cli.exe") do echo %%~zf %%~nxf
