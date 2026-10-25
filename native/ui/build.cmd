@echo off
rem Build RadarniteLauncher.exe with zig on Windows: build.cmd [out.exe]  (ZIG = path to zig.exe; flags in build.rsp;
rem the core first: ..\core\out\librncore.a, whose obj\version.gen.h, if any, sets the version resource)
setlocal
cd /d "%~dp0"
if "%ZIG%"=="" set ZIG=zig
set OUT=%~1
if "%OUT%"=="" set OUT=RadarniteLauncher.exe
if not exist ..\core\out\librncore.a (echo missing ..\core\out\librncore.a: build the core first & exit /b 1)
set RCFLAGS=
if exist ..\core\out\obj\version.gen.h set RCFLAGS=/i ..\core\out\obj /d RN_VERSION_GEN
"%ZIG%" rc %RCFLAGS% /fo app.res app.rc || exit /b 1
"%ZIG%" c++ @build.rsp -o "%OUT%" || exit /b 1
del app.res
for %%f in ("%OUT%") do echo %%~zf %%~nxf
